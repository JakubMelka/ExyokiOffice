// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "ExyokiOffice/Export.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <optional>
#include <string>
#include <vector>

namespace ExyokiOffice::Word
{

/**
 * @brief Plot type of a chart embedded in a Word document, as classified from
 * its chart part XML.
 *
 * Covers the same plot types @ref ExyokiOffice::Excel::ExcelChartType
 * builds. Chart parts using an unrecognized or unsupported plot type (for
 * example doughnut, radar, stock, or 3-D charts) are still returned by
 * @ref ExyokiOffice::Word::WordDocumentEditor::Charts with
 * @ref WordChartType::Unknown and an empty series list.
 */
enum class WordChartType
{
    /** Vertical clustered column chart. */
    Column,
    /** Horizontal clustered bar chart. */
    Bar,
    /** Line chart with a category axis. */
    Line,
    /** Pie chart. */
    Pie,
    /** Area chart with a category axis. */
    Area,
    /** XY scatter chart. */
    XyScatter,
    /** Bubble chart. */
    Bubble,
    /** The chart part's plot type was not one of the recognized values above. */
    Unknown
};

/**
 * @brief One data series read from, or supplied to update, a chart embedded
 * in a Word document.
 *
 * Unlike @ref ExyokiOffice::Excel::ExcelChartSeries, this series carries
 * literal values rather than worksheet cell ranges: an embedded chart has no
 * worksheet of its own to source data from, only its own cached values (and,
 * optionally, a separate embedded workbook reachable through
 * @ref ExyokiOffice::Word::WordDocumentEditor::GetChartEmbeddedWorkbook).
 */
struct EXYOKIOFFICE_EXPORT WordChartSeries
{
    /** @brief Series display name shown in the legend. */
    std::string Name;

    /**
     * @brief Series numeric values, in point order.
     *
     * For @ref WordChartType::XyScatter and @ref WordChartType::Bubble this
     * is the Y value series; the X values are carried in @ref categories.
     */
    std::vector<Real> Values;

    /**
     * @brief Category axis labels, in point order, or std::nullopt when the
     * series has no category reference at all.
     *
     * For @ref WordChartType::XyScatter and @ref WordChartType::Bubble,
     * where both axes are numeric, this instead carries the X value series:
     * each entry is the decimal text of one X value (see
     * @ref ExyokiOffice::Word::WordDocumentEditor::UpdateChartData for how
     * these round-trip on write).
     */
    std::optional<std::vector<std::string>> Categories;
};

/**
 * @brief Snapshot of one chart embedded in a Word document, returned by
 * @ref ExyokiOffice::Word::WordDocumentEditor::Charts.
 */
struct EXYOKIOFFICE_EXPORT WordChartInfo
{
    /**
     * @brief Relationship id of the chart part on the main document part.
     *
     * Identifies the chart for @ref
     * ExyokiOffice::Word::WordDocumentEditor::UpdateChartData,
     * GetChartEmbeddedWorkbook, and SetChartEmbeddedWorkbook. Stable for the
     * lifetime of the chart part; unrelated to its position in the document.
     */
    std::string RelationshipId;

    /** @brief Chart title, or empty when the chart has no title element. */
    std::string Title;

    /** @brief Plot type of the chart's first (and typically only) plot-type group. */
    WordChartType Type = WordChartType::Unknown;

    /**
     * @brief Series read from the chart's embedded value/label caches, in
     * document order.
     *
     * These are the cached numbers Word/Excel/PowerPoint display without
     * recalculation, not a live read of any source workbook: a chart whose
     * embedded workbook was edited without also calling @ref
     * ExyokiOffice::Word::WordDocumentEditor::UpdateChartData still
     * reports its old cached values here.
     */
    std::vector<WordChartSeries> Series;

    /**
     * @brief True when the chart part carries an embedded workbook
     * (`EmbeddedPackagePart`, typically a full `.xlsx`), accessible through
     * @ref ExyokiOffice::Word::WordDocumentEditor::GetChartEmbeddedWorkbook.
     */
    bool HasEmbeddedWorkbook = false;
};

} // namespace ExyokiOffice::Word
