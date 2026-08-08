// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "FuzzHarness.hpp"
#include "FuzzTargets.hpp"

#include "ExyokiOffice/Excel/ExcelAddress.hpp"
#include "ExyokiOffice/Excel/ExcelDocument.hpp"
#include "ExyokiOffice/Excel/ExcelFormulaEngine.hpp"
#include "ExyokiOffice/Excel/ExcelReference.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <string>
#include <string_view>

namespace ExyokiOffice::Fuzz
{

/**
 * @brief Address, range and formula parsing helpers.
 *
 * Every parser here returns std::optional or a result struct rather than
 * throwing, so the interesting property is not "does it crash" alone but
 * whether parsing and formatting agree. A reference that survives ParseA1 but
 * comes back different from ToA1() would silently rewrite user formulas during
 * a structural edit.
 */
class FormulaFuzzHelpers
{
public:
    /// Empty workbook shared by all iterations; building one per input is far too slow.
    static const Excel::ExcelDocument::Ptr& SharedDocument()
    {
        static const Excel::ExcelDocument::Ptr document = Excel::ExcelDocument::Create();
        return document;
    }

    static void CheckCellAddress(std::string_view text)
    {
        const auto parsed = Excel::CellAddress::ParseA1(text);
        if (!parsed || !parsed->IsValid())
        {
            return;
        }

        const std::string formatted = parsed->ToA1();
        const auto reparsed = Excel::CellAddress::ParseA1(formatted);
        EXYOKIOFFICE_FUZZ_CHECK(reparsed.has_value(),
                                "CellAddress::ToA1() produced text that ParseA1() rejects");
        EXYOKIOFFICE_FUZZ_CHECK(reparsed->ToA1() == formatted,
                                "CellAddress A1 parse/format round-trip is not stable");

        // R1C1 is a second spelling of the same coordinates and has to agree.
        const std::string r1c1 = parsed->ToR1C1();
        const auto fromR1C1 = Excel::CellAddress::ParseR1C1(r1c1);
        EXYOKIOFFICE_FUZZ_CHECK(fromR1C1.has_value(),
                                "CellAddress::ToR1C1() produced text that ParseR1C1() rejects");
        EXYOKIOFFICE_FUZZ_CHECK(fromR1C1->ToA1() == formatted,
                                "A1 and R1C1 spellings of one address disagree");
    }

    static void CheckCellRange(std::string_view text)
    {
        const auto parsed = Excel::CellRange::ParseA1(text);
        if (!parsed || !parsed->IsValid())
        {
            return;
        }

        const std::string formatted = parsed->ToA1();
        const auto reparsed = Excel::CellRange::ParseA1(formatted);
        EXYOKIOFFICE_FUZZ_CHECK(reparsed.has_value(),
                                "CellRange::ToA1() produced text that ParseA1() rejects");
        EXYOKIOFFICE_FUZZ_CHECK(reparsed->ToA1() == formatted,
                                "CellRange A1 parse/format round-trip is not stable");

        const auto fromR1C1 = Excel::CellRange::ParseR1C1(parsed->ToR1C1());
        EXYOKIOFFICE_FUZZ_CHECK(fromR1C1.has_value(),
                                "CellRange::ToR1C1() produced text that ParseR1C1() rejects");
        EXYOKIOFFICE_FUZZ_CHECK(fromR1C1->ToA1() == formatted,
                                "A1 and R1C1 spellings of one range disagree");
    }

    static void CheckColumnName(std::string_view text)
    {
        const auto parsed = Excel::ColumnIndex::ParseName(text);
        if (!parsed || !parsed->IsValid())
        {
            return;
        }

        const std::string name = parsed->ToName();
        const auto reparsed = Excel::ColumnIndex::ParseName(name);
        EXYOKIOFFICE_FUZZ_CHECK(reparsed.has_value(),
                                "ColumnIndex::ToName() produced a name ParseName() rejects");
        EXYOKIOFFICE_FUZZ_CHECK(reparsed->Value() == parsed->Value(),
                                "ColumnIndex name round-trip changed the column");
    }

    /**
     * @brief Rewrites references with a transform chosen from the tape.
     *
     * Inserting zero rows is a no-op by definition, so the rewritten text must
     * come back byte for byte identical. That identity is a much stronger
     * property than "did not crash" and covers the whole tokenizer: any token
     * the rewriter mis-parses and re-emits differently shows up immediately.
     */
    static void CheckRewrite(std::string_view formula, std::string_view sheetName)
    {
        const auto identity = Excel::FormulaReferenceTransform::InsertRows(1, 0);
        const auto result = Excel::FormulaReferenceRewriter::RewriteA1(formula, sheetName, identity);
        if (!result.Succeeded)
        {
            return;
        }

        EXYOKIOFFICE_FUZZ_CHECK(result.Formula == formula,
                                "identity transform (insert 0 rows) changed the formula text");
        EXYOKIOFFICE_FUZZ_CHECK(result.RewrittenReferenceCount == 0,
                                "identity transform reported rewritten references");
    }
};

int RunFormula(const UInt8* data, Size size)
{
    if (size > kMaxInputSize)
    {
        return 0;
    }

    ByteTape tape(data, size);
    const UInt8 selector = tape.NextChoice(6);
    const std::string_view sheetName = tape.NextChunk();
    const std::string_view text = tape.RestAsText();

    switch (selector)
    {
        case 0:
            FormulaFuzzHelpers::CheckCellAddress(text);
            break;
        case 1:
            FormulaFuzzHelpers::CheckCellRange(text);
            break;
        case 2:
            FormulaFuzzHelpers::CheckColumnName(text);
            break;
        case 3:
            (void)Excel::SheetCellRange::Parse(text);
            break;
        case 4:
            FormulaFuzzHelpers::CheckRewrite(text, sheetName);
            break;
        default:
        {
            const Excel::FormulaEngine engine(FormulaFuzzHelpers::SharedDocument());
            (void)engine.ValidateFormula(text);
            break;
        }
    }

    // The R1C1 entry points are only reachable through their own parsers.
    (void)Excel::CellAddress::ParseR1C1(text);
    (void)Excel::CellRange::ParseR1C1(text);

    return 0;
}

} // namespace ExyokiOffice::Fuzz
