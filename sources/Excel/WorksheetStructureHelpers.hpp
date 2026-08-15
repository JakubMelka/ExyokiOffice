// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "ExyokiOffice/Excel/ExcelDocument.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

namespace ExyokiOffice::Excel
{

class WorksheetStructureHelpers final
{
public:
    WorksheetStructureHelpers() = delete;

    static RangeOperationResult InsertRows(Worksheet& worksheet,
                                           UInt32 beforeRow,
                                           UInt32 count,
                                           FormulaReferenceUpdatePolicy formulaPolicy);
    static RangeOperationResult DeleteRows(Worksheet& worksheet,
                                           UInt32 firstRow,
                                           UInt32 count,
                                           FormulaReferenceUpdatePolicy formulaPolicy);
    static RangeOperationResult InsertColumns(Worksheet& worksheet,
                                              UInt32 beforeColumn,
                                              UInt32 count,
                                              FormulaReferenceUpdatePolicy formulaPolicy);
    static RangeOperationResult DeleteColumns(Worksheet& worksheet,
                                              UInt32 firstColumn,
                                              UInt32 count,
                                              FormulaReferenceUpdatePolicy formulaPolicy);
};

} // namespace ExyokiOffice::Excel
