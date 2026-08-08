// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "ExyokiOffice/Excel/ExcelDocument.hpp"

namespace ExyokiOffice::Excel
{

class WorksheetMergeHelpers final
{
public:
    WorksheetMergeHelpers() = delete;

    static std::vector<CellRange> MergedRanges(const Worksheet& worksheet);
    static RangeOperationResult MergeRange(Worksheet& worksheet, CellRange range);
    static RangeOperationResult UnmergeRange(Worksheet& worksheet, CellRange range);
};

} // namespace ExyokiOffice::Excel
