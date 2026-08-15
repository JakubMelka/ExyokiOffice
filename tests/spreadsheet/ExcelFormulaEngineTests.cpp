// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "doctest.h"

#include "ExyokiOffice/Excel/ExcelDocument.hpp"
#include "ExyokiOffice/Excel/ExcelFormulaEngine.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <cmath>

using namespace ExyokiOffice::Excel;

class FormulaEngineTestHelpers final
{
public:
    FormulaEngineTestHelpers() = delete;

    static CellAddress Address(std::string_view text)
    {
        const auto address = CellAddress::ParseA1(text);
        REQUIRE(address);
        return *address;
    }

    static CellRange Range(std::string_view text)
    {
        const auto range = CellRange::ParseA1(text);
        REQUIRE(range);
        return *range;
    }

    /** Evaluates ad-hoc formula text and requires a successful number result. */
    static ExyokiOffice::Real Number(const FormulaEngine& engine, std::string_view formula)
    {
        const auto result = engine.EvaluateFormula(formula);
        REQUIRE(result.Succeeded());
        REQUIRE(result.Value.Kind() == FormulaValueKind::Number);
        return *result.Value.NumberValue();
    }

    /** Evaluates ad-hoc formula text and requires a successful text result. */
    static std::string Text(const FormulaEngine& engine, std::string_view formula)
    {
        const auto result = engine.EvaluateFormula(formula);
        REQUIRE(result.Succeeded());
        REQUIRE(result.Value.Kind() == FormulaValueKind::Text);
        return result.Value.TextValue();
    }

    /** Evaluates ad-hoc formula text and requires a successful boolean result. */
    static bool Boolean(const FormulaEngine& engine, std::string_view formula)
    {
        const auto result = engine.EvaluateFormula(formula);
        REQUIRE(result.Succeeded());
        REQUIRE(result.Value.Kind() == FormulaValueKind::Boolean);
        return *result.Value.BooleanValue();
    }

    /** Evaluates ad-hoc formula text and requires a worksheet error result. */
    static FormulaErrorCode Error(const FormulaEngine& engine, std::string_view formula)
    {
        const auto result = engine.EvaluateFormula(formula);
        REQUIRE(result.Succeeded());
        REQUIRE(result.Value.IsError());
        return result.Value.ErrorCode();
    }
};

using H = FormulaEngineTestHelpers;

TEST_SUITE("ExcelFormulaEngineTests")
{

    // ---------------------------------------------------------------------------
    // Group 1: validation and parser diagnostics
    // ---------------------------------------------------------------------------

    TEST_CASE("ValidateFormula accepts well-formed formulas [unit] [excel] [excel-formula-engine]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        FormulaEngine engine(editor->GetDocument());
        REQUIRE(engine.IsValid());

        CHECK(engine.ValidateFormula("=SUM(A1:A3)"));
        CHECK(engine.ValidateFormula("SUM(A1:A3)"));
        CHECK(engine.ValidateFormula("=1+2*3^-2"));
        CHECK(engine.ValidateFormula("=\"quoted \"\"text\"\"\"&A1"));
        CHECK(engine.ValidateFormula("='My Sheet'!B2+Sheet1!C3"));
        CHECK(engine.ValidateFormula("={1,2;3,4}"));
        CHECK(engine.ValidateFormula("=_xlfn.IFNA(A1,0)"));
        CHECK(engine.ValidateFormula("=IF(A1>0,\"yes\",\"no\")"));
        CHECK(engine.ValidateFormula("=SUM(A:A)"));
        CHECK(engine.ValidateFormula("=SUM(1:3)"));
        CHECK(engine.ValidateFormula("=SUM((A1,B3))"));
        CHECK(engine.ValidateFormula("=50%"));
        CHECK(engine.ValidateFormula("=#REF!+1"));
    }

    TEST_CASE("ValidateFormula reports diagnostics with positions [unit] [excel] [excel-formula-engine]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        FormulaEngine engine(editor->GetDocument());

        SUBCASE("unbalanced parenthesis")
        {
            const auto result = engine.ValidateFormula("=SUM(A1:A3");
            REQUIRE_FALSE(result.Succeeded());
            CHECK(result.Status.Error == FormulaEngineError::ParseError);
            REQUIRE_FALSE(result.Diagnostics.empty());
            // Offsets are relative to the text after the leading '='.
            CHECK(result.Diagnostics.front().Offset == 9);
        }
        SUBCASE("empty formula")
        {
            const auto result = engine.ValidateFormula("=");
            CHECK_FALSE(result.Succeeded());
        }
        SUBCASE("trailing garbage")
        {
            const auto result = engine.ValidateFormula("=1+2)");
            REQUIRE_FALSE(result.Succeeded());
            CHECK(result.Diagnostics.front().Offset == 3);
            CHECK(result.Diagnostics.front().Length == 1);
        }
        SUBCASE("unterminated string literal")
        {
            CHECK_FALSE(engine.ValidateFormula("=\"abc"));
        }
        SUBCASE("unknown function")
        {
            const auto result = engine.ValidateFormula("=NOSUCHFUNCTION(1)");
            REQUIRE_FALSE(result.Succeeded());
            CHECK(result.Diagnostics.front().Message.find("NOSUCHFUNCTION") != std::string::npos);
        }
        SUBCASE("wrong argument count")
        {
            CHECK_FALSE(engine.ValidateFormula("=ABS(1,2)"));
            CHECK_FALSE(engine.ValidateFormula("=ABS()"));
        }
        SUBCASE("jagged array literal")
        {
            CHECK_FALSE(engine.ValidateFormula("={1,2;3}"));
        }
        SUBCASE("reference in array literal")
        {
            CHECK_FALSE(engine.ValidateFormula("={A1,2}"));
        }
    }

    // ---------------------------------------------------------------------------
    // Group 2: operators and coercion
    // ---------------------------------------------------------------------------

    TEST_CASE("Operator precedence matches Excel [unit] [excel] [excel-formula-engine]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        FormulaEngine engine(editor->GetDocument());

        CHECK(H::Number(engine, "=1+2*3") == doctest::Approx(7.0));
        // Unary minus binds tighter than the power operator.
        CHECK(H::Number(engine, "=-2^2") == doctest::Approx(4.0));
        CHECK(H::Number(engine, "=0-2^2") == doctest::Approx(-4.0));
        // The power operator is left-associative in Excel.
        CHECK(H::Number(engine, "=2^3^2") == doctest::Approx(64.0));
        CHECK(H::Number(engine, "=2^-1") == doctest::Approx(0.5));
        // Postfix percent.
        CHECK(H::Number(engine, "=50%") == doctest::Approx(0.5));
        CHECK(H::Number(engine, "=-50%") == doctest::Approx(-0.5));
        CHECK(H::Number(engine, "=200%%") == doctest::Approx(0.02));
        // Concatenation binds tighter than comparison.
        CHECK(H::Boolean(engine, "=\"a\"&\"b\"=\"ab\""));
        // Comparison chains evaluate left to right.
        CHECK_FALSE(H::Boolean(engine, "=1<2<3")); // (1<2) -> TRUE; TRUE<3 is FALSE (boolean > number)
    }

    TEST_CASE("Coercion follows Excel rules [unit] [excel] [excel-formula-engine]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        FormulaEngine engine(editor->GetDocument());

        CHECK(H::Number(engine, "=\"3\"+4") == doctest::Approx(7.0));
        CHECK(H::Number(engine, "=TRUE+1") == doctest::Approx(2.0));
        CHECK(H::Number(engine, "=FALSE*10") == doctest::Approx(0.0));
        CHECK(H::Error(engine, "=\"abc\"+1") == FormulaErrorCode::Value);
        CHECK(H::Text(engine, "=1&2") == "12");
        CHECK(H::Text(engine, "=TRUE&\"!\"") == "TRUE!");
        // Case-insensitive text comparison.
        CHECK(H::Boolean(engine, "=\"Hello\"=\"HELLO\""));
        // Cross-type ordering: numbers < text < booleans.
        CHECK(H::Boolean(engine, "=\"a\">100"));
        CHECK(H::Boolean(engine, "=TRUE>\"zzz\""));
        // Blank cells behave as zero in arithmetic.
        auto sheet = editor->FirstWorksheet();
        REQUIRE(sheet);
        CHECK(H::Number(engine, "=A1+5") == doctest::Approx(5.0));
        CHECK(H::Boolean(engine, "=A1=0"));
        CHECK(H::Boolean(engine, "=A1=\"\""));
    }

    // ---------------------------------------------------------------------------
    // Group 3: worksheet errors as values
    // ---------------------------------------------------------------------------

    TEST_CASE("All worksheet error values are produced and propagate [unit] [excel] [excel-formula-engine]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        auto sheet = editor->FirstWorksheet();
        REQUIRE(sheet->SetCellNumber(1, 1, 1.0)); // A1
        REQUIRE(sheet->SetCellNumber(2, 1, 2.0)); // A2
        FormulaEngine engine(editor->GetDocument());

        CHECK(H::Error(engine, "=1/0") == FormulaErrorCode::Div0);
        CHECK(H::Error(engine, "=SQRT(-1)") == FormulaErrorCode::Num);
        CHECK(H::Error(engine, "=UNKNOWNNAME") == FormulaErrorCode::Name);
        CHECK(H::Error(engine, "=NOSUCHFUNCTION(1)") == FormulaErrorCode::Name);
        CHECK(H::Error(engine, "=MATCH(99,A1:A2,0)") == FormulaErrorCode::NA);
        CHECK(H::Error(engine, "=#REF!+1") == FormulaErrorCode::Ref);
        CHECK(H::Error(engine, "=\"abc\"*2") == FormulaErrorCode::Value);
        // Disjoint intersection produces #NULL!.
        CHECK(H::Error(engine, "=A1:A2 B5:B6") == FormulaErrorCode::Null);
        // Overlapping intersection works.
        CHECK(H::Number(engine, "=SUM(A1:A2 A2:A3)") == doctest::Approx(2.0));

        // Errors propagate through operators and eager functions.
        CHECK(H::Error(engine, "=1+1/0") == FormulaErrorCode::Div0);
        CHECK(H::Error(engine, "=ABS(1/0)") == FormulaErrorCode::Div0);
        CHECK(H::Error(engine, "=SUM(1,1/0)") == FormulaErrorCode::Div0);

        // Lazy special forms contain errors.
        CHECK(H::Number(engine, "=IF(TRUE,1,1/0)") == doctest::Approx(1.0));
        CHECK(H::Number(engine, "=IFERROR(1/0,42)") == doctest::Approx(42.0));
        CHECK(H::Number(engine, "=IFERROR(5,42)") == doctest::Approx(5.0));
        CHECK(H::Number(engine, "=IFNA(MATCH(99,A1:A2,0),7)") == doctest::Approx(7.0));
        CHECK(H::Error(engine, "=IFNA(1/0,7)") == FormulaErrorCode::Div0);

        // Error text round-trips.
        CHECK(FormulaErrorText(FormulaErrorCode::Div0) == "#DIV/0!");
        CHECK(ParseFormulaErrorText("#N/A") == FormulaErrorCode::NA);
        CHECK_FALSE(ParseFormulaErrorText("#BOGUS!"));
    }

    // ---------------------------------------------------------------------------
    // Group 4: function library
    // ---------------------------------------------------------------------------

    TEST_CASE("Math and trigonometry functions [unit] [excel] [excel-formula-engine]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        FormulaEngine engine(editor->GetDocument());

        CHECK(H::Number(engine, "=SUM(1,2,3)") == doctest::Approx(6.0));
        CHECK(H::Number(engine, "=PRODUCT(2,3,4)") == doctest::Approx(24.0));
        CHECK(H::Number(engine, "=ABS(-3)") == doctest::Approx(3.0));
        CHECK(H::Number(engine, "=SIGN(-7)") == doctest::Approx(-1.0));
        CHECK(H::Number(engine, "=INT(-1.5)") == doctest::Approx(-2.0));
        CHECK(H::Number(engine, "=TRUNC(-1.5)") == doctest::Approx(-1.0));
        CHECK(H::Number(engine, "=ROUND(2.5,0)") == doctest::Approx(3.0));
        CHECK(H::Number(engine, "=ROUND(-2.5,0)") == doctest::Approx(-3.0));
        CHECK(H::Number(engine, "=ROUND(1.2345,2)") == doctest::Approx(1.23));
        CHECK(H::Number(engine, "=ROUNDUP(1.21,1)") == doctest::Approx(1.3));
        CHECK(H::Number(engine, "=ROUNDDOWN(1.29,1)") == doctest::Approx(1.2));
        CHECK(H::Number(engine, "=MOD(10,3)") == doctest::Approx(1.0));
        CHECK(H::Number(engine, "=MOD(-1,3)") == doctest::Approx(2.0)); // sign of divisor
        CHECK(H::Number(engine, "=POWER(2,10)") == doctest::Approx(1024.0));
        CHECK(H::Number(engine, "=SQRT(16)") == doctest::Approx(4.0));
        CHECK(H::Number(engine, "=EXP(0)") == doctest::Approx(1.0));
        CHECK(H::Number(engine, "=LN(EXP(2))") == doctest::Approx(2.0));
        CHECK(H::Number(engine, "=LOG(8,2)") == doctest::Approx(3.0));
        CHECK(H::Number(engine, "=LOG10(1000)") == doctest::Approx(3.0));
        CHECK(H::Number(engine, "=PI()") == doctest::Approx(3.14159265358979));
        CHECK(H::Number(engine, "=SIN(0)") == doctest::Approx(0.0));
        CHECK(H::Number(engine, "=COS(0)") == doctest::Approx(1.0));
        CHECK(H::Number(engine, "=DEGREES(PI())") == doctest::Approx(180.0));
        CHECK(H::Number(engine, "=RADIANS(180)") == doctest::Approx(3.14159265358979));
        CHECK(H::Number(engine, "=ATAN2(1,1)") == doctest::Approx(0.785398163397448));
        CHECK(H::Number(engine, "=CEILING(2.5,1)") == doctest::Approx(3.0));
        CHECK(H::Number(engine, "=FLOOR(2.5,1)") == doctest::Approx(2.0));
        CHECK(H::Number(engine, "=CEILING(7,4)") == doctest::Approx(8.0));
        CHECK(H::Number(engine, "=EVEN(1.5)") == doctest::Approx(2.0));
        CHECK(H::Number(engine, "=EVEN(-1)") == doctest::Approx(-2.0));
        CHECK(H::Number(engine, "=ODD(2)") == doctest::Approx(3.0));
        CHECK(H::Error(engine, "=MOD(1,0)") == FormulaErrorCode::Div0);
        CHECK(H::Error(engine, "=POWER(0,0)") == FormulaErrorCode::Num);
        CHECK(H::Error(engine, "=LN(0)") == FormulaErrorCode::Num);

        const ExyokiOffice::Real random = H::Number(engine, "=RAND()");
        CHECK(random >= 0.0);
        CHECK(random < 1.0);
        const ExyokiOffice::Real randomBetween = H::Number(engine, "=RANDBETWEEN(5,10)");
        CHECK(randomBetween >= 5.0);
        CHECK(randomBetween <= 10.0);
        CHECK(randomBetween == std::floor(randomBetween));
    }

    TEST_CASE("Statistical and aggregate functions over ranges [unit] [excel] [excel-formula-engine]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        auto sheet = editor->FirstWorksheet();
        // A1:A5 = 10, 20, 30, text, TRUE; B1:B3 = 2, 4, 6
        REQUIRE(sheet->SetCellNumber(1, 1, 10.0));
        REQUIRE(sheet->SetCellNumber(2, 1, 20.0));
        REQUIRE(sheet->SetCellNumber(3, 1, 30.0));
        REQUIRE(sheet->SetCellText(4, 1, "text"));
        REQUIRE(sheet->SetCellBoolean(H::Address("A5"), true));
        REQUIRE(sheet->SetCellNumber(1, 2, 2.0));
        REQUIRE(sheet->SetCellNumber(2, 2, 4.0));
        REQUIRE(sheet->SetCellNumber(3, 2, 6.0));
        FormulaEngine engine(editor->GetDocument());

        // Text and booleans inside ranges are ignored by numeric aggregates.
        CHECK(H::Number(engine, "=SUM(A1:A5)") == doctest::Approx(60.0));
        CHECK(H::Number(engine, "=AVERAGE(A1:A5)") == doctest::Approx(20.0));
        CHECK(H::Number(engine, "=COUNT(A1:A5)") == doctest::Approx(3.0));
        CHECK(H::Number(engine, "=COUNTA(A1:A5)") == doctest::Approx(5.0));
        CHECK(H::Number(engine, "=COUNTBLANK(A1:A6)") == doctest::Approx(1.0));
        CHECK(H::Number(engine, "=MAX(A1:A5)") == doctest::Approx(30.0));
        CHECK(H::Number(engine, "=MIN(A1:A5)") == doctest::Approx(10.0));
        CHECK(H::Number(engine, "=MEDIAN(A1:A3)") == doctest::Approx(20.0));
        CHECK(H::Number(engine, "=LARGE(A1:A3,1)") == doctest::Approx(30.0));
        CHECK(H::Number(engine, "=SMALL(A1:A3,2)") == doctest::Approx(20.0));
        CHECK(H::Number(engine, "=STDEV(B1:B3)") == doctest::Approx(2.0));
        CHECK(H::Number(engine, "=STDEVP(B1:B3)") == doctest::Approx(std::sqrt(8.0 / 3.0)));
        CHECK(H::Number(engine, "=VAR(B1:B3)") == doctest::Approx(4.0));
        CHECK(H::Number(engine, "=VARP(B1:B3)") == doctest::Approx(8.0 / 3.0));
        CHECK(H::Number(engine, "=MODE(1,2,2,3)") == doctest::Approx(2.0));
        CHECK(H::Error(engine, "=MODE(1,2,3)") == FormulaErrorCode::NA);
        CHECK(H::Error(engine, "=AVERAGE(C1:C4)") == FormulaErrorCode::Div0);
        // Whole-column references stay cheap and correct.
        CHECK(H::Number(engine, "=SUM(B:B)") == doctest::Approx(12.0));
        CHECK(H::Number(engine, "=SUMPRODUCT(A1:A3,B1:B3)") == doctest::Approx(10.0 * 2 + 20.0 * 4 + 30.0 * 6));
    }

    TEST_CASE("Conditional aggregation with criteria [unit] [excel] [excel-formula-engine]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        auto sheet = editor->FirstWorksheet();
        // A: category, B: value
        REQUIRE(sheet->SetCellText(1, 1, "apple"));
        REQUIRE(sheet->SetCellText(2, 1, "banana"));
        REQUIRE(sheet->SetCellText(3, 1, "apricot"));
        REQUIRE(sheet->SetCellText(4, 1, "banana"));
        REQUIRE(sheet->SetCellNumber(1, 2, 10.0));
        REQUIRE(sheet->SetCellNumber(2, 2, 20.0));
        REQUIRE(sheet->SetCellNumber(3, 2, 30.0));
        REQUIRE(sheet->SetCellNumber(4, 2, 40.0));
        FormulaEngine engine(editor->GetDocument());

        CHECK(H::Number(engine, "=COUNTIF(A1:A4,\"banana\")") == doctest::Approx(2.0));
        CHECK(H::Number(engine, "=COUNTIF(A1:A4,\"ap*\")") == doctest::Approx(2.0));
        CHECK(H::Number(engine, "=COUNTIF(A1:A4,\"?anana\")") == doctest::Approx(2.0));
        CHECK(H::Number(engine, "=COUNTIF(B1:B4,\">15\")") == doctest::Approx(3.0));
        CHECK(H::Number(engine, "=COUNTIF(B1:B4,\"<>20\")") == doctest::Approx(3.0));
        CHECK(H::Number(engine, "=SUMIF(A1:A4,\"banana\",B1:B4)") == doctest::Approx(60.0));
        CHECK(H::Number(engine, "=SUMIF(B1:B4,\">=30\")") == doctest::Approx(70.0));
        CHECK(H::Number(engine, "=AVERAGEIF(A1:A4,\"banana\",B1:B4)") == doctest::Approx(30.0));
        CHECK(H::Number(engine, "=SUMIFS(B1:B4,A1:A4,\"banana\",B1:B4,\">25\")") == doctest::Approx(40.0));
        CHECK(H::Number(engine, "=COUNTIFS(A1:A4,\"banana\",B1:B4,\">25\")") == doctest::Approx(1.0));
        CHECK(H::Number(engine, "=AVERAGEIFS(B1:B4,A1:A4,\"ap*\")") == doctest::Approx(20.0));
    }

    TEST_CASE("Logical and information functions [unit] [excel] [excel-formula-engine]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        auto sheet = editor->FirstWorksheet();
        REQUIRE(sheet->SetCellNumber(1, 1, 5.0));
        REQUIRE(sheet->SetCellText(2, 1, "hi"));
        REQUIRE(sheet->SetCellError(H::Address("A3"), "#N/A"));
        FormulaEngine engine(editor->GetDocument());

        CHECK(H::Boolean(engine, "=AND(TRUE,1,5>2)"));
        CHECK_FALSE(H::Boolean(engine, "=AND(TRUE,0)"));
        CHECK(H::Boolean(engine, "=OR(FALSE,0,1)"));
        CHECK(H::Boolean(engine, "=XOR(TRUE,FALSE,FALSE)"));
        CHECK_FALSE(H::Boolean(engine, "=XOR(TRUE,TRUE)"));
        CHECK(H::Boolean(engine, "=NOT(FALSE)"));
        CHECK(H::Boolean(engine, "=TRUE()"));
        CHECK_FALSE(H::Boolean(engine, "=FALSE()"));
        // AND propagates errors even when another operand is FALSE.
        CHECK(H::Error(engine, "=AND(FALSE,1/0)") == FormulaErrorCode::Div0);

        CHECK(H::Number(engine, "=IFS(FALSE,1,TRUE,2)") == doctest::Approx(2.0));
        CHECK(H::Error(engine, "=IFS(FALSE,1,FALSE,2)") == FormulaErrorCode::NA);
        CHECK(H::Text(engine, "=SWITCH(2,1,\"one\",2,\"two\",\"other\")") == "two");
        CHECK(H::Text(engine, "=SWITCH(9,1,\"one\",2,\"two\",\"other\")") == "other");
        CHECK(H::Number(engine, "=CHOOSE(2,10,20,30)") == doctest::Approx(20.0));
        CHECK(H::Error(engine, "=CHOOSE(4,10,20,30)") == FormulaErrorCode::Value);

        CHECK(H::Boolean(engine, "=ISBLANK(Z99)"));
        CHECK_FALSE(H::Boolean(engine, "=ISBLANK(A1)"));
        CHECK(H::Boolean(engine, "=ISNUMBER(A1)"));
        CHECK(H::Boolean(engine, "=ISTEXT(A2)"));
        CHECK(H::Boolean(engine, "=ISLOGICAL(TRUE)"));
        CHECK(H::Boolean(engine, "=ISNA(A3)"));
        CHECK(H::Boolean(engine, "=ISERROR(1/0)"));
        CHECK_FALSE(H::Boolean(engine, "=ISERR(A3)")); // #N/A is not an ISERR error
        CHECK(H::Boolean(engine, "=ISEVEN(4)"));
        CHECK(H::Boolean(engine, "=ISODD(7)"));
        CHECK(H::Number(engine, "=N(TRUE)") == doctest::Approx(1.0));
        CHECK(H::Number(engine, "=N(\"text\")") == doctest::Approx(0.0));
        CHECK(H::Error(engine, "=NA()") == FormulaErrorCode::NA);
    }

    TEST_CASE("Text functions [unit] [excel] [excel-formula-engine]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        FormulaEngine engine(editor->GetDocument());

        CHECK(H::Text(engine, "=CONCATENATE(\"a\",1,TRUE)") == "a1TRUE");
        CHECK(H::Text(engine, "=CONCAT(\"x\",\"y\")") == "xy");
        CHECK(H::Text(engine, "=TEXTJOIN(\"-\",TRUE,\"a\",\"\",\"b\")") == "a-b");
        CHECK(H::Text(engine, "=TEXTJOIN(\"-\",FALSE,\"a\",\"\",\"b\")") == "a--b");
        CHECK(H::Text(engine, "=LEFT(\"hello\",2)") == "he");
        CHECK(H::Text(engine, "=LEFT(\"hello\")") == "h");
        CHECK(H::Text(engine, "=RIGHT(\"hello\",3)") == "llo");
        CHECK(H::Text(engine, "=MID(\"hello\",2,3)") == "ell");
        CHECK(H::Text(engine, "=MID(\"hello\",7,3)") == "");
        CHECK(H::Number(engine, "=LEN(\"hello\")") == doctest::Approx(5.0));
        // UTF-8: character counting, not byte counting.
        CHECK(H::Number(engine, "=LEN(\"příliš\")") == doctest::Approx(6.0));
        CHECK(H::Text(engine, "=UPPER(\"aBc\")") == "ABC");
        CHECK(H::Text(engine, "=LOWER(\"AbC\")") == "abc");
        CHECK(H::Text(engine, "=PROPER(\"hello world\")") == "Hello World");
        // Case folding reaches ASCII only, so a word opening with a letter
        // outside it keeps that letter as it stands. Word breaking, though, has
        // to see the whole character: were a byte of a UTF-8 sequence not a
        // letter, the second half of one would open a word of its own and
        // uppercase the letter behind it - "cau svete", with carons, would come
        // back as "cAu svEte". The second word still capitalizes, because its
        // first letter is ASCII.
        CHECK(H::Text(engine, "=PROPER(\"\xC4\x8D"
                              "au sv\xC4\x9B"
                              "te\")") ==
              "\xC4\x8D"
              "au Sv\xC4\x9B"
              "te");
        CHECK(H::Text(engine, "=TRIM(\"  a   b  \")") == "a b");
        CHECK(H::Text(engine, "=SUBSTITUTE(\"aaa\",\"a\",\"b\",2)") == "aba");
        CHECK(H::Text(engine, "=SUBSTITUTE(\"aaa\",\"a\",\"b\")") == "bbb");
        CHECK(H::Text(engine, "=REPLACE(\"abcdef\",2,3,\"X\")") == "aXef");
        CHECK(H::Number(engine, "=FIND(\"l\",\"hello\")") == doctest::Approx(3.0));
        CHECK(H::Number(engine, "=FIND(\"l\",\"hello\",4)") == doctest::Approx(4.0));
        CHECK(H::Error(engine, "=FIND(\"L\",\"hello\")") == FormulaErrorCode::Value);   // case-sensitive
        CHECK(H::Number(engine, "=SEARCH(\"L\",\"hello\")") == doctest::Approx(3.0));   // case-insensitive
        CHECK(H::Number(engine, "=SEARCH(\"l?o\",\"hello\")") == doctest::Approx(3.0)); // wildcards
        CHECK(H::Text(engine, "=REPT(\"ab\",3)") == "ababab");
        CHECK(H::Number(engine, "=VALUE(\"12.5\")") == doctest::Approx(12.5));
        CHECK(H::Number(engine, "=VALUE(\"50%\")") == doctest::Approx(0.5));
        CHECK(H::Error(engine, "=VALUE(\"abc\")") == FormulaErrorCode::Value);
        CHECK(H::Boolean(engine, "=EXACT(\"abc\",\"abc\")"));
        CHECK_FALSE(H::Boolean(engine, "=EXACT(\"abc\",\"ABC\")"));
        CHECK(H::Text(engine, "=CHAR(65)") == "A");
        CHECK(H::Number(engine, "=CODE(\"A\")") == doctest::Approx(65.0));
    }

    TEST_CASE("TEXT formats numbers with the supported code subset [unit] [excel] [excel-formula-engine]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        FormulaEngine engine(editor->GetDocument());

        CHECK(H::Text(engine, "=TEXT(1234.567,\"0.00\")") == "1234.57");
        CHECK(H::Text(engine, "=TEXT(1234.567,\"#,##0.0\")") == "1,234.6");
        CHECK(H::Text(engine, "=TEXT(0.75,\"0%\")") == "75%");
        CHECK(H::Text(engine, "=TEXT(0.5,\"0.0%\")") == "50.0%");
        CHECK(H::Text(engine, "=TEXT(-5,\"0;(0)\")") == "(5)");
        CHECK(H::Text(engine, "=TEXT(12345,\"0.00E+00\")") == "1.23E+04");
        CHECK(H::Text(engine, "=TEXT(7,\"General\")") == "7");
        CHECK(H::Text(engine, "=TEXT(DATE(2024,3,15),\"yyyy-mm-dd\")") == "2024-03-15");
        CHECK(H::Text(engine, "=TEXT(DATE(2024,3,15),\"d.m.yyyy\")") == "15.3.2024");
        CHECK(H::Text(engine, "=TEXT(DATE(2024,3,15),\"mmmm\")") == "March");
        CHECK(H::Text(engine, "=TEXT(DATE(2024,3,15),\"ddd\")") == "Fri");
        CHECK(H::Text(engine, "=TEXT(TIME(14,5,9),\"hh:mm:ss\")") == "14:05:09");
        CHECK(H::Text(engine, "=TEXT(TIME(14,5,9),\"h:mm AM/PM\")") == "2:05 PM");
        // Unsupported codes report #VALUE! instead of silently misformatting.
        CHECK(H::Error(engine, "=TEXT(1,\"[Red]0\")") == FormulaErrorCode::Value);
    }

    TEST_CASE("TEXT routes text values through the @ placeholder [unit] [excel] [excel-formula-engine]")
    {
        // A format code has up to four sections and the fourth one is the only
        // one that applies to text. Getting the selection wrong is invisible on
        // numbers and wrong on every text value, so each rule is pinned here:
        // a lone section applies, a fourth section wins over the first, and a
        // code with no text placeholder leaves the value alone rather than
        // formatting it as a number - which is what Excel does.
        auto editor = ExcelDocumentEditor::CreateNew();
        FormulaEngine engine(editor->GetDocument());

        CHECK(H::Text(engine, "=TEXT(\"abc\",\"@\")") == "abc");
        // Backslash escapes and quoted runs are both literal text around the
        // placeholder.
        CHECK(H::Text(engine, "=TEXT(\"abc\",\"\\[@\\]\")") == "[abc]");
        CHECK(H::Text(engine, "=TEXT(\"abc\",\"\"\"<\"\"@\"\">\"\"\")") == "<abc>");
        // Four sections: positive;negative;zero;text - the last one is the one
        // a text value is formatted with.
        CHECK(H::Text(engine, "=TEXT(\"abc\",\"0;-0;0;\\(@\\)\")") == "(abc)");
        // Three sections carry no text section at all, so the first one is used
        // and, having no `@`, leaves the value untouched.
        CHECK(H::Text(engine, "=TEXT(\"abc\",\"0.00;-0.00;0\")") == "abc");
        CHECK(H::Text(engine, "=TEXT(\"abc\",\"0.00\")") == "abc");
        CHECK(H::Text(engine, "=TEXT(\"abc\",\"General\")") == "abc");
        CHECK(H::Text(engine, "=TEXT(\"\",\"@\")").empty());
        // A section that mixes the text placeholder with number tokens has no
        // meaning for a text value; #VALUE! beats inventing one.
        CHECK(H::Error(engine, "=TEXT(\"abc\",\"@0.0\")") == FormulaErrorCode::Value);
        // Numbers keep taking the numeric path even when a text section exists.
        CHECK(H::Text(engine, "=TEXT(5,\"0.0;-0.0;0;\\(@\\)\")") == "5.0");
        // The number a text argument would coerce to is not what TEXT applies
        // the format to: a numeric-looking string is still a string here.
        CHECK(H::Text(engine, "=TEXT(\"12\",\"0.00\")") == "12");
    }

    TEST_CASE("Lookup functions [unit] [excel] [excel-formula-engine]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        auto sheet = editor->FirstWorksheet();
        // Table A1:C3: key, name, price
        REQUIRE(sheet->SetCellNumber(1, 1, 1.0));
        REQUIRE(sheet->SetCellText(1, 2, "one"));
        REQUIRE(sheet->SetCellNumber(1, 3, 1.5));
        REQUIRE(sheet->SetCellNumber(2, 1, 5.0));
        REQUIRE(sheet->SetCellText(2, 2, "five"));
        REQUIRE(sheet->SetCellNumber(2, 3, 5.5));
        REQUIRE(sheet->SetCellNumber(3, 1, 9.0));
        REQUIRE(sheet->SetCellText(3, 2, "nine"));
        REQUIRE(sheet->SetCellNumber(3, 3, 9.5));
        FormulaEngine engine(editor->GetDocument());

        CHECK(H::Text(engine, "=VLOOKUP(5,A1:C3,2,FALSE)") == "five");
        CHECK(H::Number(engine, "=VLOOKUP(5,A1:C3,3,FALSE)") == doctest::Approx(5.5));
        CHECK(H::Error(engine, "=VLOOKUP(4,A1:C3,2,FALSE)") == FormulaErrorCode::NA);
        // Approximate match takes the largest key <= subject.
        CHECK(H::Text(engine, "=VLOOKUP(7,A1:C3,2,TRUE)") == "five");
        CHECK(H::Text(engine, "=VLOOKUP(9,A1:C3,2)") == "nine");
        CHECK(H::Error(engine, "=VLOOKUP(0,A1:C3,2,TRUE)") == FormulaErrorCode::NA);
        CHECK(H::Error(engine, "=VLOOKUP(5,A1:C3,4,FALSE)") == FormulaErrorCode::Ref);

        CHECK(H::Number(engine, "=HLOOKUP(1,A1:C1,1,FALSE)") == doctest::Approx(1.0));

        CHECK(H::Number(engine, "=MATCH(5,A1:A3,0)") == doctest::Approx(2.0));
        CHECK(H::Number(engine, "=MATCH(7,A1:A3,1)") == doctest::Approx(2.0));
        CHECK(H::Number(engine, "=MATCH(\"nine\",B1:B3,0)") == doctest::Approx(3.0));
        CHECK(H::Number(engine, "=MATCH(\"n*\",B1:B3,0)") == doctest::Approx(3.0));

        CHECK(H::Text(engine, "=INDEX(A1:C3,2,2)") == "five");
        CHECK(H::Error(engine, "=INDEX(A1:C3,4,1)") == FormulaErrorCode::Ref);
        CHECK(H::Number(engine, "=INDEX(A1:A3,3)") == doctest::Approx(9.0));
        // Classic INDEX/MATCH combination.
        CHECK(H::Number(engine, "=INDEX(C1:C3,MATCH(\"five\",B1:B3,0))") == doctest::Approx(5.5));

        CHECK(H::Text(engine, "=LOOKUP(6,A1:A3,B1:B3)") == "five");

        CHECK(H::Number(engine, "=ROWS(A1:C3)") == doctest::Approx(3.0));
        CHECK(H::Number(engine, "=COLUMNS(A1:C3)") == doctest::Approx(3.0));
        CHECK(H::Number(engine, "=ROW(B7)") == doctest::Approx(7.0));
        CHECK(H::Number(engine, "=COLUMN(B7)") == doctest::Approx(2.0));

        CHECK(H::Text(engine, "=OFFSET(A1,1,1)") == "five");
        CHECK(H::Number(engine, "=SUM(OFFSET(A1,0,0,3,1))") == doctest::Approx(15.0));
        CHECK(H::Error(engine, "=OFFSET(A1,-1,0)") == FormulaErrorCode::Ref);

        CHECK(H::Text(engine, "=INDIRECT(\"B2\")") == "five");
        CHECK(H::Number(engine, "=SUM(INDIRECT(\"A1:A3\"))") == doctest::Approx(15.0));
        CHECK(H::Error(engine, "=INDIRECT(\"not a ref\")") == FormulaErrorCode::Ref);
        CHECK(H::Error(engine, "=INDIRECT(\"R1C1\",FALSE)") == FormulaErrorCode::Ref);
    }

    TEST_CASE("Date and time functions use 1900-system serials [unit] [excel] [excel-formula-engine]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        FormulaEngine engine(editor->GetDocument());

        CHECK(H::Number(engine, "=DATE(1900,1,1)") == doctest::Approx(1.0));
        CHECK(H::Number(engine, "=DATE(1900,2,28)") == doctest::Approx(59.0));
        // The fictitious 1900 leap day is preserved: 1900-03-01 is serial 61.
        CHECK(H::Number(engine, "=DATE(1900,3,1)") == doctest::Approx(61.0));
        CHECK(H::Number(engine, "=DATE(2024,3,15)") == doctest::Approx(45366.0));
        // Month overflow normalizes like Excel.
        CHECK(H::Number(engine, "=DATE(2023,13,1)") == H::Number(engine, "=DATE(2024,1,1)"));
        // Years below 1900 are offset by 1900.
        CHECK(H::Number(engine, "=DATE(24,3,15)") == H::Number(engine, "=DATE(1924,3,15)"));

        CHECK(H::Number(engine, "=YEAR(DATE(2024,3,15))") == doctest::Approx(2024.0));
        CHECK(H::Number(engine, "=MONTH(DATE(2024,3,15))") == doctest::Approx(3.0));
        CHECK(H::Number(engine, "=DAY(DATE(2024,3,15))") == doctest::Approx(15.0));

        CHECK(H::Number(engine, "=TIME(12,0,0)") == doctest::Approx(0.5));
        CHECK(H::Number(engine, "=HOUR(TIME(14,30,45))") == doctest::Approx(14.0));
        CHECK(H::Number(engine, "=MINUTE(TIME(14,30,45))") == doctest::Approx(30.0));
        CHECK(H::Number(engine, "=SECOND(TIME(14,30,45))") == doctest::Approx(45.0));

        CHECK(H::Number(engine, "=DATEVALUE(\"2024-03-15\")") == doctest::Approx(45366.0));
        CHECK(H::Number(engine, "=DATEVALUE(\"3/15/2024\")") == doctest::Approx(45366.0));
        CHECK(H::Number(engine, "=TIMEVALUE(\"2024-03-15T06:00:00\")") == doctest::Approx(0.25));
        CHECK(H::Error(engine, "=DATEVALUE(\"nonsense\")") == FormulaErrorCode::Value);

        // 2024-03-15 was a Friday.
        CHECK(H::Number(engine, "=WEEKDAY(DATE(2024,3,15))") == doctest::Approx(6.0));
        CHECK(H::Number(engine, "=WEEKDAY(DATE(2024,3,15),2)") == doctest::Approx(5.0));
        CHECK(H::Number(engine, "=WEEKDAY(DATE(2024,3,15),3)") == doctest::Approx(4.0));
        CHECK(H::Number(engine, "=WEEKNUM(DATE(2024,1,1))") == doctest::Approx(1.0));

        CHECK(H::Number(engine, "=EDATE(DATE(2024,1,31),1)") == H::Number(engine, "=DATE(2024,2,29)"));
        CHECK(H::Number(engine, "=EOMONTH(DATE(2024,2,5),0)") == H::Number(engine, "=DATE(2024,2,29)"));
        CHECK(H::Number(engine, "=EOMONTH(DATE(2024,1,15),-1)") == H::Number(engine, "=DATE(2023,12,31)"));
        CHECK(H::Number(engine, "=DAYS(DATE(2024,3,15),DATE(2024,3,1))") == doctest::Approx(14.0));

        // TODAY and NOW return plausible current serials.
        const ExyokiOffice::Real today = H::Number(engine, "=TODAY()");
        CHECK(today == std::floor(today));
        CHECK(today > 46000.0); // after mid-2025
        const ExyokiOffice::Real now = H::Number(engine, "=NOW()");
        CHECK(now >= today);
        CHECK(now < today + 1.0);
    }

    TEST_CASE("Financial functions [unit] [excel] [excel-formula-engine]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        FormulaEngine engine(editor->GetDocument());

        // Known Excel values (documentation examples).
        CHECK(H::Number(engine, "=PMT(0.08/12,10,10000)") == doctest::Approx(-1037.03).epsilon(0.001));
        CHECK(H::Number(engine, "=FV(0.06/12,10,-200,-500,1)") == doctest::Approx(2581.4).epsilon(0.001));
        CHECK(H::Number(engine, "=PV(0.08/12,240,500)") == doctest::Approx(-59777.15).epsilon(0.001));
        CHECK(H::Number(engine, "=NPER(0.12/12,-100,-1000,10000,1)") == doctest::Approx(59.6738657).epsilon(0.001));
        CHECK(H::Number(engine, "=IPMT(0.1/12,1,36,8000)") == doctest::Approx(-66.67).epsilon(0.001));
        CHECK(H::Number(engine, "=PPMT(0.1/12,1,36,8000)") ==
              doctest::Approx(H::Number(engine, "=PMT(0.1/12,36,8000)") + 66.6667).epsilon(0.001));
        CHECK(H::Number(engine, "=RATE(48,-200,8000)") == doctest::Approx(0.0077014725).epsilon(0.0001));
        CHECK(H::Number(engine, "=NPV(0.1,-10000,3000,4200,6800)") == doctest::Approx(1188.44).epsilon(0.001));
        CHECK(H::Number(engine, "=PMT(0,12,-1200)") == doctest::Approx(100.0));
        CHECK(H::Error(engine, "=NPV(-1,100)") == FormulaErrorCode::Div0);

        auto sheet = editor->FirstWorksheet();
        REQUIRE(sheet->SetCellNumber(1, 1, -70000.0));
        REQUIRE(sheet->SetCellNumber(2, 1, 12000.0));
        REQUIRE(sheet->SetCellNumber(3, 1, 15000.0));
        REQUIRE(sheet->SetCellNumber(4, 1, 18000.0));
        REQUIRE(sheet->SetCellNumber(5, 1, 21000.0));
        REQUIRE(sheet->SetCellNumber(6, 1, 26000.0));
        CHECK(H::Number(engine, "=IRR(A1:A6)") == doctest::Approx(0.0866309).epsilon(0.0001));
        CHECK(H::Error(engine, "=IRR(A2:A6)") == FormulaErrorCode::Num); // no sign change
    }

    // ---------------------------------------------------------------------------
    // Group 5: references
    // ---------------------------------------------------------------------------

    TEST_CASE("Cross-sheet and qualified references [unit] [excel] [excel-formula-engine]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        auto first = editor->FirstWorksheet();
        REQUIRE(editor->RenameWorksheet(0, "Data"));
        auto report = editor->AddWorksheet("My Report");
        REQUIRE(report);
        REQUIRE(first->SetCellNumber(1, 1, 11.0));
        REQUIRE(first->SetCellNumber(2, 1, 22.0));
        REQUIRE(report->SetCellNumber(1, 1, 100.0));
        FormulaEngine engine(editor->GetDocument());

        // Unqualified references resolve on the evaluation sheet.
        const auto onData = engine.EvaluateFormula("=A1*2", "Data");
        REQUIRE(onData.Succeeded());
        CHECK(*onData.Value.NumberValue() == doctest::Approx(22.0));
        const auto onReport = engine.EvaluateFormula("=A1*2", "My Report");
        REQUIRE(onReport.Succeeded());
        CHECK(*onReport.Value.NumberValue() == doctest::Approx(200.0));

        // Qualified references reach other sheets; quoting works.
        CHECK(H::Number(engine, "=Data!A2") == doctest::Approx(22.0));
        CHECK(H::Number(engine, "='My Report'!A1") == doctest::Approx(100.0));
        CHECK(H::Number(engine, "=SUM(Data!A1:A2)+'My Report'!A1") == doctest::Approx(133.0));
        // Absolute markers do not change the value.
        CHECK(H::Number(engine, "=Data!$A$2") == doctest::Approx(22.0));
        // Unknown sheets produce #REF!.
        CHECK(H::Error(engine, "=Nope!A1") == FormulaErrorCode::Ref);
        // Sheet-name matching is case-insensitive.
        CHECK(H::Number(engine, "=DATA!a1") == doctest::Approx(11.0));
        // Union inside an aggregate.
        CHECK(H::Number(engine, "=SUM((Data!A1,Data!A2))") == doctest::Approx(33.0));
        // External workbook references are documented as #REF!.
        CHECK(H::Error(engine, "=[Book1.xlsx]Sheet1!A1") == FormulaErrorCode::Ref);
        // Defined names evaluate to #NAME?.
        CHECK(H::Error(engine, "=MyDefinedName+1") == FormulaErrorCode::Name);

        // Unknown evaluation sheet is an engine-level error, not a value.
        const auto unknownSheet = engine.EvaluateFormula("=1", "Missing");
        CHECK_FALSE(unknownSheet.Succeeded());
        CHECK(unknownSheet.Status.Error == FormulaEngineError::UnknownSheet);
    }

    TEST_CASE("Every stored cell kind reaches a formula as the value it holds [unit] [excel] [excel-formula-engine]")
    {
        // SpreadsheetML stores a cell's type next to its text: `b` for booleans,
        // `e` for errors, `d` for an ISO date, `inlineStr` for text kept in the
        // sheet, and a formula cell keeps its last result with a type of its
        // own. A workbook written by Excel is full of these, and a reference to
        // one has to arrive in the expression as that value - reading a boolean
        // cell as the number 0, or an error cell as text, is wrong in a way that
        // no single formula makes obvious.
        auto editor = ExcelDocumentEditor::CreateNew();
        auto sheet = editor->FirstWorksheet();
        REQUIRE(sheet);

        REQUIRE(sheet->SetCellValue(H::Address("A1"), ExcelCellValue::Boolean(true)));
        REQUIRE(sheet->SetCellValue(H::Address("A2"), ExcelCellValue::Error("#DIV/0!")));
        REQUIRE(sheet->SetCellValue(H::Address("A3"), ExcelCellValue::InlineString("inline text")));
        REQUIRE(sheet->SetCellValue(H::Address("A4"), ExcelCellValue::DateTimeText("2024-03-15T00:00:00")));
        REQUIRE(sheet->SetCellValue(H::Address("A5"), ExcelCellValue::NumberText("2.5")));
        // SetCellText interns into the shared string table, so A6 is the
        // SharedString kind rather than an inline one.
        REQUIRE(sheet->SetCellText(H::Address("A6"), "shared text"));

        FormulaEngine engine(editor->GetDocument());

        CHECK(H::Boolean(engine, "=A1"));
        CHECK(H::Number(engine, "=A1+1") == doctest::Approx(2.0));
        CHECK(H::Error(engine, "=A2") == FormulaErrorCode::Div0);
        // An error cell poisons whatever reads it, rather than counting as zero.
        CHECK(H::Error(engine, "=A2+1") == FormulaErrorCode::Div0);
        CHECK(H::Text(engine, "=A3") == "inline text");
        CHECK(H::Text(engine, "=A6") == "shared text");
        // A `d`-typed cell arrives as its serial, so date arithmetic works on it.
        CHECK(H::Number(engine, "=A4") == doctest::Approx(45366.0));
        CHECK(H::Number(engine, "=YEAR(A4)") == doctest::Approx(2024.0));
        CHECK(H::Number(engine, "=A5*2") == doctest::Approx(5.0));

        // Aggregates apply Excel's rules to the mixture: SUM ignores text and
        // booleans stored in cells, COUNT counts only numbers, COUNTA counts
        // everything that is not blank, and an error still propagates.
        REQUIRE(sheet->SetCellValue(H::Address("C1"), ExcelCellValue::Number(10.0)));
        REQUIRE(sheet->SetCellValue(H::Address("C2"), ExcelCellValue::Boolean(true)));
        REQUIRE(sheet->SetCellValue(H::Address("C3"), ExcelCellValue::InlineString("text")));
        REQUIRE(sheet->SetCellValue(H::Address("C4"), ExcelCellValue::Blank()));
        REQUIRE(sheet->SetCellValue(H::Address("C5"), ExcelCellValue::Number(5.0)));
        CHECK(H::Number(engine, "=SUM(C1:C5)") == doctest::Approx(15.0));
        CHECK(H::Number(engine, "=COUNT(C1:C5)") == doctest::Approx(2.0));
        CHECK(H::Number(engine, "=COUNTA(C1:C5)") == doctest::Approx(4.0));
    }

    TEST_CASE("A formula cell's cached value is what a reference reads [unit] [excel] [excel-formula-engine]")
    {
        // The engine does not recompute the whole workbook to answer one
        // reference: a formula cell contributes its stored result, in the type
        // the file says it has. That is what makes reading an untouched workbook
        // cheap, and it is also why the cached-type dispatch has to be right for
        // every type - a cached boolean read as text would compare wrongly
        // without ever looking like an error.
        auto editor = ExcelDocumentEditor::CreateNew();
        auto sheet = editor->FirstWorksheet();
        REQUIRE(sheet);

        REQUIRE(sheet->SetCellFormula(H::Address("A1"), "=1+1", FormulaCachedValueKind::Number, "2"));
        REQUIRE(sheet->SetCellFormula(H::Address("A2"), "=1=1", FormulaCachedValueKind::Boolean, "1"));
        REQUIRE(sheet->SetCellFormula(H::Address("A3"), "=\"a\"&\"b\"", FormulaCachedValueKind::String, "ab"));
        REQUIRE(sheet->SetCellFormula(H::Address("A4"), "=1/0", FormulaCachedValueKind::Error, "#DIV/0!"));
        REQUIRE(sheet->SetCellFormula(H::Address("A5"), "=TODAY()", FormulaCachedValueKind::DateTime,
                                      "2024-03-15T00:00:00"));
        REQUIRE(sheet->SetCellFormula(H::Address("A6"), "=1+1", FormulaCachedValueKind::None, ""));

        FormulaEngine engine(editor->GetDocument());

        CHECK(H::Number(engine, "=A1*10") == doctest::Approx(20.0));
        CHECK(H::Boolean(engine, "=A2"));
        CHECK(H::Text(engine, "=A3") == "ab");
        CHECK(H::Error(engine, "=A4") == FormulaErrorCode::Div0);
        CHECK(H::Number(engine, "=A5") == doctest::Approx(45366.0));
        // No cached value at all reads as blank, which is zero in arithmetic -
        // not as a recomputation of the formula behind it.
        CHECK(H::Number(engine, "=A6+7") == doctest::Approx(7.0));

        // EvaluateCell is the opposite contract: it recomputes and ignores the
        // stale cache, so the two answers for the same cell differ on purpose.
        const auto recomputed = engine.EvaluateCell("Sheet1", H::Address("A6"));
        REQUIRE(recomputed.Succeeded());
        CHECK(*recomputed.Value.NumberValue() == doctest::Approx(2.0));
    }

    // ---------------------------------------------------------------------------
    // Group 6: EvaluateCell, shared formulas, array formulas
    // ---------------------------------------------------------------------------

    TEST_CASE("EvaluateCell recomputes stored formulas [unit] [excel] [excel-formula-engine]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        auto sheet = editor->FirstWorksheet();
        REQUIRE(sheet->SetCellNumber(1, 1, 10.0));
        REQUIRE(sheet->SetCellNumber(2, 1, 32.0));
        // Stale cached value 999 is ignored by EvaluateCell.
        REQUIRE(sheet->SetCellFormula(H::Address("A3"), "=SUM(A1:A2)",
                                      FormulaCachedValueKind::Number, "999"));
        FormulaEngine engine(editor->GetDocument());

        const auto result = engine.EvaluateCell("Sheet1", H::Address("A3"));
        REQUIRE(result.Succeeded());
        CHECK(*result.Value.NumberValue() == doctest::Approx(42.0));

        const auto notFormula = engine.EvaluateCell("Sheet1", H::Address("A1"));
        CHECK_FALSE(notFormula.Succeeded());
        CHECK(notFormula.Status.Error == FormulaEngineError::NotAFormulaCell);

        const auto badSheet = engine.EvaluateCell("Missing", H::Address("A3"));
        CHECK(badSheet.Status.Error == FormulaEngineError::UnknownSheet);

        const auto badAddress = engine.EvaluateCell("Sheet1", CellAddress());
        CHECK(badAddress.Status.Error == FormulaEngineError::InvalidAddress);
    }

    TEST_CASE("Shared formulas expand with per-cell offsets [unit] [excel] [excel-formula-engine]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        auto sheet = editor->FirstWorksheet();
        REQUIRE(sheet->SetCellNumber(1, 1, 1.0));
        REQUIRE(sheet->SetCellNumber(2, 1, 2.0));
        REQUIRE(sheet->SetCellNumber(3, 1, 3.0));
        // Shared group B1:B3 anchored at B1 with formula A1*10.
        REQUIRE(sheet->SetCellFormula(H::Address("B1"),
                                      ExcelCellValue::Formula(CellFormulaValue::Shared("A1*10", 0, "B1:B3"))
                                          .FormulaValue()));
        REQUIRE(sheet->SetCellFormula(H::Address("B2"),
                                      CellFormulaValue::SharedDependent(0)));
        REQUIRE(sheet->SetCellFormula(H::Address("B3"),
                                      CellFormulaValue::SharedDependent(0)));
        FormulaEngine engine(editor->GetDocument());

        const auto b1 = engine.EvaluateCell("Sheet1", H::Address("B1"));
        REQUIRE(b1.Succeeded());
        CHECK(*b1.Value.NumberValue() == doctest::Approx(10.0));
        // The dependent cells evaluate the anchor expression shifted down.
        const auto b2 = engine.EvaluateCell("Sheet1", H::Address("B2"));
        REQUIRE(b2.Succeeded());
        CHECK(*b2.Value.NumberValue() == doctest::Approx(20.0));
        const auto b3 = engine.EvaluateCell("Sheet1", H::Address("B3"));
        REQUIRE(b3.Succeeded());
        CHECK(*b3.Value.NumberValue() == doctest::Approx(30.0));
    }

    TEST_CASE("Array formulas evaluate with CSE semantics [unit] [excel] [excel-formula-engine]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        auto sheet = editor->FirstWorksheet();
        REQUIRE(sheet->SetCellNumber(1, 1, 1.0));
        REQUIRE(sheet->SetCellNumber(2, 1, 2.0));
        REQUIRE(sheet->SetCellNumber(3, 1, 3.0));
        REQUIRE(sheet->SetCellNumber(1, 2, 10.0));
        REQUIRE(sheet->SetCellNumber(2, 2, 20.0));
        REQUIRE(sheet->SetCellNumber(3, 2, 30.0));
        FormulaEngine engine(editor->GetDocument());

        // Array literal arithmetic broadcasts elementwise.
        const auto literal = engine.EvaluateFormula("={1,2;3,4}*2");
        REQUIRE(literal.Succeeded());
        // Without array context a scalar consumer sees the top-left element.
        CHECK(*literal.Value.NumberValue() == doctest::Approx(2.0));

        // A stored array formula evaluates its whole matrix.
        REQUIRE(sheet->SetCellFormula(H::Address("C1"),
                                      CellFormulaValue::Array("A1:A3*B1:B3", "C1:C3")));
        const auto matrix = engine.EvaluateCell("Sheet1", H::Address("C1"));
        REQUIRE(matrix.Succeeded());
        REQUIRE(matrix.Value.Kind() == FormulaValueKind::Array);
        CHECK(matrix.Value.RowCount() == 3);
        CHECK(matrix.Value.ColumnCount() == 1);
        CHECK(*matrix.Value.At(0, 0).NumberValue() == doctest::Approx(10.0));
        CHECK(*matrix.Value.At(1, 0).NumberValue() == doctest::Approx(40.0));
        CHECK(*matrix.Value.At(2, 0).NumberValue() == doctest::Approx(90.0));

        // SUM over an elementwise product needs no CSE helper functions.
        REQUIRE(sheet->SetCellFormula(H::Address("D1"),
                                      CellFormulaValue::Array("SUM(A1:A3*B1:B3)", "D1")));
        const auto total = engine.EvaluateCell("Sheet1", H::Address("D1"));
        REQUIRE(total.Succeeded());
        CHECK(*total.Value.NumberValue() == doctest::Approx(140.0));
    }

    // ---------------------------------------------------------------------------
    // Group 7: circular references
    // ---------------------------------------------------------------------------

    TEST_CASE("Circular references are detected and reported [unit] [excel] [excel-formula-engine]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        auto sheet = editor->FirstWorksheet();
        REQUIRE(editor->RenameWorksheet(0, "Main"));
        auto other = editor->AddWorksheet("Other");
        REQUIRE(other);

        // Self-loop: A1 = A1+1.
        REQUIRE(sheet->SetCellFormula(H::Address("A1"), "=A1+1"));
        // Two-cycle: B1 <-> B2.
        REQUIRE(sheet->SetCellFormula(H::Address("B1"), "=B2"));
        REQUIRE(sheet->SetCellFormula(H::Address("B2"), "=B1"));
        // Cross-sheet three-cycle: Main!C1 -> Other!A1 -> Main!C2 -> Main!C1.
        REQUIRE(sheet->SetCellFormula(H::Address("C1"), "=Other!A1"));
        REQUIRE(other->SetCellFormula(H::Address("A1"), "=Main!C2"));
        REQUIRE(sheet->SetCellFormula(H::Address("C2"), "=C1"));
        // An independent healthy formula.
        REQUIRE(sheet->SetCellNumber(1, 4, 5.0)); // D1
        REQUIRE(sheet->SetCellFormula(H::Address("D2"), "=D1*2"));

        FormulaEngine engine(editor->GetDocument());
        const auto cycles = engine.FindCircularReferences();
        REQUIRE(cycles.size() == 3);

        ExyokiOffice::Size totalMembers = 0;
        bool foundSelfLoop = false;
        bool foundCrossSheet = false;
        for (const auto& cycle : cycles)
        {
            totalMembers += cycle.size();
            if (cycle.size() == 1)
            {
                foundSelfLoop = true;
                CHECK(cycle.front().ToFormula() == "Main!A1");
            }
            for (const auto& member : cycle)
            {
                if (member.Sheet == "Other")
                {
                    foundCrossSheet = true;
                }
            }
        }
        CHECK(totalMembers == 6);
        CHECK(foundSelfLoop);
        CHECK(foundCrossSheet);
    }

    TEST_CASE("Recalculate keeps cycle caches and updates healthy cells [unit] [excel] [excel-formula-engine]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        auto sheet = editor->FirstWorksheet();
        REQUIRE(sheet->SetCellFormula(H::Address("A1"), "=A2", FormulaCachedValueKind::Number, "111"));
        REQUIRE(sheet->SetCellFormula(H::Address("A2"), "=A1", FormulaCachedValueKind::Number, "222"));
        REQUIRE(sheet->SetCellNumber(1, 2, 5.0)); // B1
        REQUIRE(sheet->SetCellFormula(H::Address("B2"), "=B1*2"));
        // B3 depends on the cycle and uses its previous cached values.
        REQUIRE(sheet->SetCellFormula(H::Address("B3"), "=A1+A2"));

        FormulaEngine engine(editor->GetDocument());
        const auto result = engine.Recalculate();
        REQUIRE(result.Succeeded());
        REQUIRE(result.CircularReferenceCycles.size() == 1);
        CHECK(result.CircularReferenceCycles.front().size() == 2);

        // Cycle members keep their previous cached values.
        CHECK(sheet->GetCellFormula(H::Address("A1"))->CachedText == "111");
        CHECK(sheet->GetCellFormula(H::Address("A2"))->CachedText == "222");
        // Healthy cells were recalculated.
        CHECK(sheet->GetCellFormula(H::Address("B2"))->CachedText == "10");
        // Dependents of the cycle evaluate from the previous cached values.
        CHECK(sheet->GetCellFormula(H::Address("B3"))->CachedText == "333");
        CHECK(result.RecalculatedCellCount == 2);
    }

    // ---------------------------------------------------------------------------
    // Group 8: recalculation ordering
    // ---------------------------------------------------------------------------

    TEST_CASE("Recalculation follows dependency order regardless of storage order [unit] [excel] [excel-formula-engine]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        auto sheet = editor->FirstWorksheet();
        // Chain stored "backwards": A1 depends on A2 depends on A3.
        REQUIRE(sheet->SetCellFormula(H::Address("A1"), "=A2+1"));
        REQUIRE(sheet->SetCellFormula(H::Address("A2"), "=A3+1"));
        REQUIRE(sheet->SetCellNumber(3, 1, 40.0)); // A3

        FormulaEngine engine(editor->GetDocument());
        const auto result = engine.Recalculate();
        REQUIRE(result.Succeeded());
        CHECK(result.RecalculatedCellCount == 2);
        CHECK(result.CircularReferenceCycles.empty());
        CHECK(sheet->GetCellFormula(H::Address("A2"))->CachedText == "41");
        CHECK(sheet->GetCellFormula(H::Address("A1"))->CachedText == "42");
    }

    TEST_CASE("Recalculation orders cross-sheet dependencies [unit] [excel] [excel-formula-engine]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        REQUIRE(editor->RenameWorksheet(0, "Input"));
        auto input = editor->FirstWorksheet();
        auto output = editor->AddWorksheet("Output");
        REQUIRE(output);
        REQUIRE(input->SetCellNumber(1, 1, 3.0));
        REQUIRE(output->SetCellFormula(H::Address("A1"), "=Input!B1*10"));
        REQUIRE(input->SetCellFormula(H::Address("B1"), "=A1+1"));

        FormulaEngine engine(editor->GetDocument());
        const auto result = engine.Recalculate();
        REQUIRE(result.Succeeded());
        CHECK(result.RecalculatedCellCount == 2);
        CHECK(input->GetCellFormula(H::Address("B1"))->CachedText == "4");
        CHECK(output->GetCellFormula(H::Address("A1"))->CachedText == "40");
    }

    TEST_CASE("RecalculateSheet writes only the requested sheet [unit] [excel] [excel-formula-engine]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        REQUIRE(editor->RenameWorksheet(0, "First"));
        auto first = editor->FirstWorksheet();
        auto second = editor->AddWorksheet("Second");
        REQUIRE(second);
        REQUIRE(first->SetCellNumber(1, 1, 2.0));
        REQUIRE(first->SetCellFormula(H::Address("A2"), "=A1*10"));
        REQUIRE(second->SetCellFormula(H::Address("A1"), "=First!A1*100"));

        FormulaEngine engine(editor->GetDocument());
        const auto result = engine.RecalculateSheet("Second");
        REQUIRE(result.Succeeded());
        CHECK(result.RecalculatedCellCount == 1);
        CHECK(second->GetCellFormula(H::Address("A1"))->CachedText == "200");
        // The other sheet's cache is untouched.
        CHECK(first->GetCellFormula(H::Address("A2"))->CachedKind == FormulaCachedValueKind::None);

        const auto missing = engine.RecalculateSheet("Missing");
        CHECK_FALSE(missing.Succeeded());
        CHECK(missing.Status.Error == FormulaEngineError::UnknownSheet);
    }

    TEST_CASE("Recalculation resolves INDIRECT dependencies with an extra pass [unit] [excel] [excel-formula-engine]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        auto sheet = editor->FirstWorksheet();
        REQUIRE(sheet->SetCellNumber(1, 1, 7.0)); // A1
        REQUIRE(sheet->SetCellFormula(H::Address("B1"), "=A1*2"));
        // INDIRECT hides the dependency on B1 from static analysis.
        REQUIRE(sheet->SetCellFormula(H::Address("C1"), "=INDIRECT(\"B\"&1)+1"));

        FormulaEngine engine(editor->GetDocument());
        const auto result = engine.Recalculate();
        REQUIRE(result.Succeeded());
        CHECK(sheet->GetCellFormula(H::Address("B1"))->CachedText == "14");
        CHECK(sheet->GetCellFormula(H::Address("C1"))->CachedText == "15");
    }

    // ---------------------------------------------------------------------------
    // Group 9: round-trip persistence
    // ---------------------------------------------------------------------------

    TEST_CASE("Recalculated cached values survive a package round-trip [unit] [excel] [excel-formula-engine]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        auto sheet = editor->FirstWorksheet();
        REQUIRE(sheet->SetCellNumber(1, 1, 2.5));
        REQUIRE(sheet->SetCellNumber(2, 1, 4.0));
        REQUIRE(sheet->SetCellFormula(H::Address("B1"), "=A1*A2"));        // number
        REQUIRE(sheet->SetCellFormula(H::Address("B2"), "=\"ab\"&\"c\"")); // text
        REQUIRE(sheet->SetCellFormula(H::Address("B3"), "=A1>A2"));        // boolean
        REQUIRE(sheet->SetCellFormula(H::Address("B4"), "=1/0"));          // error
        REQUIRE(sheet->SetCellFormula(H::Address("B5"), "=IF(A1<A2,\"less\",\"more\")"));

        FormulaEngine engine(editor->GetDocument());
        const auto recalculation = engine.Recalculate();
        REQUIRE(recalculation.Succeeded());
        CHECK(recalculation.RecalculatedCellCount == 5);

        const auto bytes = editor->SaveToMemory();
        REQUIRE_FALSE(bytes.empty());
        auto reopened = ExcelDocumentEditor::Open(bytes);
        REQUIRE(reopened);
        auto reopenedSheet = reopened->FirstWorksheet();
        REQUIRE(reopenedSheet);

        const auto number = reopenedSheet->GetCellFormula(H::Address("B1"));
        REQUIRE(number);
        CHECK(number->CachedKind == FormulaCachedValueKind::Number);
        CHECK(number->CachedText == "10");
        const auto text = reopenedSheet->GetCellFormula(H::Address("B2"));
        REQUIRE(text);
        CHECK(text->CachedKind == FormulaCachedValueKind::String);
        CHECK(text->CachedText == "abc");
        const auto boolean = reopenedSheet->GetCellFormula(H::Address("B3"));
        REQUIRE(boolean);
        CHECK(boolean->CachedKind == FormulaCachedValueKind::Boolean);
        CHECK(boolean->CachedText == "0");
        const auto error = reopenedSheet->GetCellFormula(H::Address("B4"));
        REQUIRE(error);
        CHECK(error->CachedKind == FormulaCachedValueKind::Error);
        CHECK(error->CachedText == "#DIV/0!");
        const auto conditional = reopenedSheet->GetCellFormula(H::Address("B5"));
        REQUIRE(conditional);
        CHECK(conditional->CachedText == "less");

        // The reopened workbook recalculates to the same values.
        FormulaEngine reopenedEngine(reopened->GetDocument());
        const auto cell = reopenedEngine.EvaluateCell("Sheet1", H::Address("B1"));
        REQUIRE(cell.Succeeded());
        CHECK(*cell.Value.NumberValue() == doctest::Approx(10.0));
    }

    TEST_CASE("Recalculation distributes array results and drops calcChain [unit] [excel] [excel-formula-engine]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        auto sheet = editor->FirstWorksheet();
        REQUIRE(sheet->SetCellNumber(1, 1, 1.0));
        REQUIRE(sheet->SetCellNumber(2, 1, 2.0));
        REQUIRE(sheet->SetCellNumber(3, 1, 3.0));
        REQUIRE(sheet->SetCellFormula(H::Address("B1"),
                                      CellFormulaValue::Array("A1:A3*10", "B1:B3")));

        FormulaEngine engine(editor->GetDocument());
        const auto result = engine.Recalculate();
        REQUIRE(result.Succeeded());

        // The anchor keeps its formula with the cached top-left element.
        const auto anchor = sheet->GetCellFormula(H::Address("B1"));
        REQUIRE(anchor);
        CHECK(anchor->Kind == CellFormulaKind::Array);
        CHECK(anchor->CachedText == "10");
        // The remaining range cells receive plain cached values.
        const auto b2 = sheet->GetCellValue(H::Address("B2"));
        REQUIRE(b2);
        CHECK(b2->Kind() == CellValueKind::Number);
        CHECK(b2->Text() == "20");
        const auto b3 = sheet->GetCellValue(H::Address("B3"));
        REQUIRE(b3);
        CHECK(b3->Text() == "30");

        // Formulas depending on distributed array cells see fresh values.
        REQUIRE(sheet->SetCellFormula(H::Address("C1"), "=SUM(B1:B3)"));
        const auto second = engine.Recalculate();
        REQUIRE(second.Succeeded());
        CHECK(sheet->GetCellFormula(H::Address("C1"))->CachedText == "60");
    }

    // ---------------------------------------------------------------------------
    // Group 10: custom functions
    // ---------------------------------------------------------------------------

    TEST_CASE("Custom functions register per engine [unit] [excel] [excel-formula-engine]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        auto sheet = editor->FirstWorksheet();
        REQUIRE(sheet->SetCellNumber(1, 1, 21.0));
        FormulaEngine engine(editor->GetDocument());

        FormulaFunctionSpec spec;
        spec.MinimumArgumentCount = 1;
        spec.MaximumArgumentCount = 1;
        REQUIRE(engine.RegisterFunction("DoubleIt", spec,
                                        [](FormulaFunctionContext&, std::span<const FormulaValue> arguments)
                                        {
                                            const auto number = arguments[0].NumberValue();
                                            if (!number)
                                            {
                                                return FormulaValue::Error(FormulaErrorCode::Value);
                                            }
                                            return FormulaValue::Number(*number * 2.0);
                                        }));

        CHECK(engine.IsFunctionRegistered("DOUBLEIT"));
        CHECK(engine.IsFunctionRegistered("doubleit"));
        // Case-insensitive invocation, cell references arrive dereferenced.
        CHECK(H::Number(engine, "=doubleit(A1)") == doctest::Approx(42.0));
        CHECK(H::Number(engine, "=DOUBLEIT(5)+1") == doctest::Approx(11.0));
        // Arity is enforced.
        CHECK(H::Error(engine, "=DOUBLEIT(1,2)") == FormulaErrorCode::Value);

        // A separate engine is unaffected.
        FormulaEngine other(editor->GetDocument());
        CHECK_FALSE(other.IsFunctionRegistered("DOUBLEIT"));
        CHECK(H::Error(other, "=DOUBLEIT(1)") == FormulaErrorCode::Name);

        // Built-ins can be overridden per engine.
        FormulaFunctionSpec sumSpec;
        sumSpec.MinimumArgumentCount = 0;
        REQUIRE(engine.RegisterFunction("SUM", sumSpec,
                                        [](FormulaFunctionContext&, std::span<const FormulaValue>)
                                        {
                                            return FormulaValue::Number(-1.0);
                                        }));
        CHECK(H::Number(engine, "=SUM(1,2)") == doctest::Approx(-1.0));
        CHECK(H::Number(other, "=SUM(1,2)") == doctest::Approx(3.0));

        // Invalid registrations are rejected.
        CHECK_FALSE(engine.RegisterFunction("", spec, [](FormulaFunctionContext&, std::span<const FormulaValue>)
                                            { return FormulaValue(); }));
        CHECK_FALSE(engine.RegisterFunction("1BAD", spec,
                                            [](FormulaFunctionContext&, std::span<const FormulaValue>)
                                            {
                                                return FormulaValue();
                                            }));
        CHECK_FALSE(engine.RegisterFunction("NULLFN", spec, nullptr));

        // FunctionNames lists built-ins and the custom registration.
        const auto names = engine.FunctionNames();
        CHECK(std::find(names.begin(), names.end(), "DOUBLEIT") != names.end());
        CHECK(std::find(names.begin(), names.end(), "VLOOKUP") != names.end());
        CHECK(names.size() > 100);
    }

    TEST_CASE("FormulaValue conversions and display text [unit] [excel] [excel-formula-engine]")
    {
        CHECK(FormulaValue().Kind() == FormulaValueKind::Blank);
        CHECK(FormulaValue::Number(1.5).ToDisplayText() == "1.5");
        CHECK(FormulaValue::Text("x").ToDisplayText() == "x");
        CHECK(FormulaValue::Boolean(true).ToDisplayText() == "TRUE");
        CHECK(FormulaValue::Error(FormulaErrorCode::NA).ToDisplayText() == "#N/A");
        CHECK(FormulaValue().ToDisplayText().empty());

        const auto matrix = FormulaValue::Array(2, 2,
                                                {FormulaValue::Number(1), FormulaValue::Number(2),
                                                 FormulaValue::Number(3), FormulaValue::Number(4)});
        REQUIRE(matrix.Kind() == FormulaValueKind::Array);
        CHECK(*matrix.At(1, 0).NumberValue() == doctest::Approx(3.0));
        CHECK(matrix.At(5, 5).IsError());
        CHECK(matrix.ToDisplayText() == "1");
        // Dimension mismatch collapses to #VALUE!.
        CHECK(FormulaValue::Array(2, 2, {FormulaValue::Number(1)}).IsError());

        const auto cellValue = FormulaValue::Number(2.5).ToCellValue();
        CHECK(cellValue.Kind() == CellValueKind::Number);
        CHECK(cellValue.Text() == "2.5");
        CHECK(FormulaValue::Error(FormulaErrorCode::Div0).ToCellValue().Text() == "#DIV/0!");

        // A detached engine reports engine-level failure.
        FormulaEngine detached;
        CHECK_FALSE(detached.IsValid());
        const auto result = detached.EvaluateFormula("=1+1");
        CHECK(result.Status.Error == FormulaEngineError::InvalidDocument);
    }

    // Excel's worksheet functions answer bad input with a worksheet error, not
    // with an exception or a wrong number, and a workbook that recalculates to
    // a different error than Excel shows is silently wrong. Each case below
    // pins one domain rule next to a nearby success, so the test documents the
    // boundary rather than just the failure.
    TEST_CASE("Logarithms reject their whole invalid domain [unit] [excel] [excel-formula-engine]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        FormulaEngine engine(editor->GetDocument());

        // LN and LOG10: zero and negative arguments have no logarithm.
        CHECK(H::Number(engine, "=LN(EXP(2))") == doctest::Approx(2.0));
        CHECK(H::Error(engine, "=LN(0)") == FormulaErrorCode::Num);
        CHECK(H::Error(engine, "=LN(-1)") == FormulaErrorCode::Num);
        CHECK(H::Number(engine, "=LOG10(1000)") == doctest::Approx(3.0));
        CHECK(H::Error(engine, "=LOG10(0)") == FormulaErrorCode::Num);

        // LOG: the argument, the base and the degenerate base 1 fail
        // independently of one another. Base 1 is Excel's division by
        // ln(1) = 0, so it answers #DIV/0!, not #NUM!.
        CHECK(H::Number(engine, "=LOG(100)") == doctest::Approx(2.0));
        CHECK(H::Number(engine, "=LOG(8,2)") == doctest::Approx(3.0));
        CHECK(H::Error(engine, "=LOG(0,2)") == FormulaErrorCode::Num);
        CHECK(H::Error(engine, "=LOG(-10,2)") == FormulaErrorCode::Num);
        CHECK(H::Error(engine, "=LOG(10,0)") == FormulaErrorCode::Num);
        CHECK(H::Error(engine, "=LOG(10,-2)") == FormulaErrorCode::Num);
        CHECK(H::Error(engine, "=LOG(10,1)") == FormulaErrorCode::Div0);

        // An error in either argument wins over the domain check.
        CHECK(H::Error(engine, "=LOG(1/0,2)") == FormulaErrorCode::Div0);
        CHECK(H::Error(engine, "=LOG(10,1/0)") == FormulaErrorCode::Div0);
    }

    TEST_CASE("LARGE and SMALL enforce the rank against the population [unit] [excel] [excel-formula-engine]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        auto sheet = editor->FirstWorksheet();
        REQUIRE(sheet->SetCellNumber(1, 1, 5.0)); // A1
        REQUIRE(sheet->SetCellNumber(2, 1, 3.0)); // A2
        REQUIRE(sheet->SetCellNumber(3, 1, 9.0)); // A3
        FormulaEngine engine(editor->GetDocument());

        CHECK(H::Number(engine, "=LARGE(A1:A3,1)") == doctest::Approx(9.0));
        CHECK(H::Number(engine, "=SMALL(A1:A3,1)") == doctest::Approx(3.0));
        // The rank is floored, so 2.9 still means the second value.
        CHECK(H::Number(engine, "=LARGE(A1:A3,2.9)") == doctest::Approx(5.0));

        // Rank below one, rank beyond the population, and an empty population
        // are three different ways to ask for a value that is not there.
        CHECK(H::Error(engine, "=LARGE(A1:A3,0)") == FormulaErrorCode::Num);
        CHECK(H::Error(engine, "=SMALL(A1:A3,0.5)") == FormulaErrorCode::Num);
        CHECK(H::Error(engine, "=LARGE(A1:A3,4)") == FormulaErrorCode::Num);
        CHECK(H::Error(engine, "=SMALL(C1:C9,1)") == FormulaErrorCode::Num);

        // An error rank propagates instead of being floored.
        CHECK(H::Error(engine, "=LARGE(A1:A3,1/0)") == FormulaErrorCode::Div0);
    }

    TEST_CASE("Criteria aggregates validate their references and pairs [unit] [excel] [excel-formula-engine]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        auto sheet = editor->FirstWorksheet();
        // A1:A4 = 1, 5, 10, text; B1:B4 = 2, 4, 8, 16
        REQUIRE(sheet->SetCellNumber(1, 1, 1.0));
        REQUIRE(sheet->SetCellNumber(2, 1, 5.0));
        REQUIRE(sheet->SetCellNumber(3, 1, 10.0));
        REQUIRE(sheet->SetCellText(4, 1, "text"));
        REQUIRE(sheet->SetCellNumber(1, 2, 2.0));
        REQUIRE(sheet->SetCellNumber(2, 2, 4.0));
        REQUIRE(sheet->SetCellNumber(3, 2, 8.0));
        REQUIRE(sheet->SetCellNumber(4, 2, 16.0));
        FormulaEngine engine(editor->GetDocument());

        // The happy paths the failure cases below contrast against.
        CHECK(H::Number(engine, "=COUNTIF(A1:A4,\">2\")") == doctest::Approx(2.0));
        CHECK(H::Number(engine, "=SUMIF(A1:A3,\">2\",B1:B3)") == doctest::Approx(12.0));
        CHECK(H::Number(engine, "=SUMIFS(B1:B3,A1:A3,\">2\")") == doctest::Approx(12.0));
        CHECK(H::Number(engine, "=COUNTIFS(A1:A3,\">2\",B1:B3,\">4\")") == doctest::Approx(1.0));

        // The criteria range must be exactly one reference area.
        CHECK(H::Error(engine, "=COUNTIF(5,\">2\")") == FormulaErrorCode::Value);
        CHECK(H::Error(engine, "=COUNTIF((A1:A2,B1:B2),\">2\")") == FormulaErrorCode::Value);
        CHECK(H::Error(engine, "=SUMIF(A1:A3,\">2\",7)") == FormulaErrorCode::Value);

        // *IFS pairs: a criteria range without its criterion is malformed, and
        // every range must share the shape of the first one. SUMIF is the
        // contrast: there the value range is realigned, not rejected.
        CHECK(H::Error(engine, "=COUNTIFS(A1:A3,\">2\",B1:B3)") == FormulaErrorCode::Value);
        CHECK(H::Error(engine, "=COUNTIFS(A1:A3,\">2\",B1:B2,\">0\")") == FormulaErrorCode::Value);
        CHECK(H::Error(engine, "=SUMIFS(B1:B2,A1:A3,\">2\")") == FormulaErrorCode::Value);
        CHECK(H::Number(engine, "=SUMIF(A1:A3,\">2\",B1:B1)") == doctest::Approx(12.0));

        // An error criterion propagates - except #N/A, which is a matchable
        // value: Excel lets a criterion select #N/A cells, so it must pass
        // through here and simply match nothing in an error-free sheet.
        CHECK(H::Error(engine, "=COUNTIF(A1:A3,1/0)") == FormulaErrorCode::Div0);
        CHECK(H::Error(engine, "=SUMIFS(B1:B3,A1:A3,#REF!)") == FormulaErrorCode::Ref);
        CHECK(H::Number(engine, "=COUNTIF(A1:A3,NA())") == doctest::Approx(0.0));

        // Whole-column and whole-row references clip to stored content instead
        // of iterating a million rows.
        CHECK(H::Number(engine, "=COUNTIF(A:A,\">2\")") == doctest::Approx(2.0));
        CHECK(H::Number(engine, "=COUNTIF(1:1,\">0\")") == doctest::Approx(2.0));

        // No match: COUNT answers zero, AVERAGE has nothing to divide.
        CHECK(H::Number(engine, "=COUNTIF(A1:A4,\">99\")") == doctest::Approx(0.0));
        CHECK(H::Error(engine, "=AVERAGEIF(A1:A4,\">99\")") == FormulaErrorCode::Div0);
    }

    TEST_CASE("Rounding survives extreme digit counts [unit] [excel] [excel-formula-engine]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        FormulaEngine engine(editor->GetDocument());

        CHECK(H::Number(engine, "=ROUND(2.5,0)") == doctest::Approx(3.0));
        CHECK(H::Number(engine, "=ROUND(-2.5,0)") == doctest::Approx(-3.0));
        CHECK(H::Number(engine, "=ROUND(15,-1)") == doctest::Approx(20.0));
        // A digit count so large that 10^digits overflows must not destroy the
        // value: the scale guard hands the number back unchanged.
        CHECK(H::Number(engine, "=ROUND(2.5,400)") == doctest::Approx(2.5));
        CHECK(H::Number(engine, "=ROUNDDOWN(2.9,400)") == doctest::Approx(2.9));
        // The band where 10^digits is still finite but value * 10^digits
        // overflows behaves the same: the rounding unit is below one ulp.
        CHECK(H::Number(engine, "=ROUND(2.5,308)") == doctest::Approx(2.5));
        CHECK(H::Number(engine, "=ROUNDDOWN(2.9,308)") == doctest::Approx(2.9));
        CHECK(H::Number(engine, "=ROUNDUP(2.9,308)") == doctest::Approx(2.9));
        // A hugely negative digit count rounds to a multiple of a power of
        // ten beyond the double range: to nearest and toward zero that is 0,
        // away from zero it overflows into #NUM! - continuously with the
        // subnormal scale band just above -324, which already behaves so.
        CHECK(H::Number(engine, "=ROUND(2.9,-400)") == doctest::Approx(0.0));
        CHECK(H::Number(engine, "=ROUND(-2.9,-400)") == doctest::Approx(0.0));
        CHECK(H::Number(engine, "=ROUNDDOWN(2.9,-400)") == doctest::Approx(0.0));
        CHECK(H::Number(engine, "=TRUNC(2.9,-400)") == doctest::Approx(0.0));
        CHECK(H::Error(engine, "=ROUNDUP(2.9,-400)") == FormulaErrorCode::Num);
        CHECK(H::Error(engine, "=ROUNDUP(-2.9,-400)") == FormulaErrorCode::Num);
        CHECK(H::Number(engine, "=ROUNDUP(0,-400)") == doctest::Approx(0.0));
        CHECK(H::Number(engine, "=ROUND(2.9,-310)") == doctest::Approx(0.0));
        CHECK(H::Error(engine, "=ROUNDUP(2.9,-310)") == FormulaErrorCode::Num);
    }

    // A result Excel cannot represent is a visible #NUM!, never an infinity
    // or NaN smuggled into a cell: every accumulator and product path has to
    // end in the same finiteness check the scalar functions already use.
    TEST_CASE("Aggregates answer overflow with #NUM! instead of infinities [unit] [excel] [excel-formula-engine]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        auto sheet = editor->FirstWorksheet();
        REQUIRE(sheet->SetCellNumber(1, 1, 9e307)); // A1
        REQUIRE(sheet->SetCellNumber(2, 1, 9e307)); // A2
        FormulaEngine engine(editor->GetDocument());

        CHECK(H::Error(engine, "=SUM(9E307,9E307)") == FormulaErrorCode::Num);
        CHECK(H::Error(engine, "=PRODUCT(1E200,1E200)") == FormulaErrorCode::Num);
        CHECK(H::Error(engine, "=AVERAGE(9E307,9E307)") == FormulaErrorCode::Num);
        CHECK(H::Error(engine, "=MEDIAN(9E307,1.6E308)") == FormulaErrorCode::Num);
        CHECK(H::Error(engine, "=VARP(1E200,-1E200)") == FormulaErrorCode::Num);
        CHECK(H::Error(engine, "=STDEVP(1E200,-1E200)") == FormulaErrorCode::Num);
        CHECK(H::Error(engine, "=SUMPRODUCT({1E200},{1E200})") == FormulaErrorCode::Num);
        CHECK(H::Error(engine, "=DEGREES(9E307)") == FormulaErrorCode::Num);
        CHECK(H::Error(engine, "=SUMIF(A1:A2,\">0\")") == FormulaErrorCode::Num);
        CHECK(H::Error(engine, "=SUMIFS(A1:A2,A1:A2,\">0\")") == FormulaErrorCode::Num);

        // The same inputs at half the magnitude stay ordinary numbers.
        CHECK(H::Number(engine, "=SUM(4E307,4E307)") == doctest::Approx(8e307));
    }

    TEST_CASE("Scalar function domains follow Excel's limits [unit] [excel] [excel-formula-engine]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        FormulaEngine engine(editor->GetDocument());

        // Excel refuses trigonometric arguments at 2^27 and above.
        CHECK(H::Number(engine, "=SIN(134217727)") == doctest::Approx(std::sin(134217727.0)));
        CHECK(H::Error(engine, "=SIN(134217728)") == FormulaErrorCode::Num);
        CHECK(H::Error(engine, "=COS(-134217728)") == FormulaErrorCode::Num);
        CHECK(H::Error(engine, "=TAN(1E300)") == FormulaErrorCode::Num);

        // A direct text argument of AVERAGEA follows scalar coercion: numeric
        // text contributes its value, unreadable text is #VALUE!. Text inside
        // a range or array constant still counts as zero.
        CHECK(H::Number(engine, "=AVERAGEA(\"3\")") == doctest::Approx(3.0));
        CHECK(H::Error(engine, "=AVERAGEA(\"abc\")") == FormulaErrorCode::Value);
        CHECK(H::Number(engine, "=AVERAGEA({1,\"abc\"})") == doctest::Approx(0.5));

        // Text inside an array constant is ignored by the logical aggregates,
        // exactly like text inside a range; only direct text is coerced.
        CHECK(H::Boolean(engine, "=AND({1,\"abc\"})") == true);
        CHECK(H::Boolean(engine, "=OR({0,\"abc\"})") == false);
        CHECK(H::Boolean(engine, "=AND(\"TRUE\",1)") == true);

        // RANDBETWEEN bounds beyond 2^53 draw in the real domain instead of
        // overflowing the integer distribution.
        const std::string huge = "=RANDBETWEEN(1,1E19)";
        for (int i = 0; i < 8; ++i)
        {
            const ExyokiOffice::Real drawn = H::Number(engine, huge);
            CHECK(drawn >= 1.0);
            CHECK(drawn <= 1e19);
            CHECK(drawn == std::floor(drawn));
        }
    }

    TEST_CASE("Custom function names follow the documented lexical rule [unit] [excel] [excel-formula-engine]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        FormulaEngine engine(editor->GetDocument());

        FormulaFunctionSpec spec;
        spec.MinimumArgumentCount = 0;
        spec.MaximumArgumentCount = 0;
        const auto constant = [](FormulaFunctionContext&, std::span<const FormulaValue>)
        {
            return FormulaValue::Number(1.0);
        };

        // ASCII letters, digits, `.` and `_`, not starting with a digit - the
        // shape of real add-in names such as MY.UDF or _xll helpers.
        for (const std::string_view accepted :
             {"MY.FUNC", "_private", "A", "z9", "R2.D2_", "_1"})
        {
            CHECK_MESSAGE(engine.RegisterFunction(accepted, spec, constant),
                          "expected accepted: ", accepted);
            CHECK(engine.IsFunctionRegistered(accepted));
        }

        // A name Excel would refuse must be refused here too, or the workbook
        // this engine writes stops opening elsewhere.
        // "\xC5\xA1" is UTF-8 for a non-ASCII letter; kept escaped so the file
        // stays ASCII (see the encoding note in CONTRIBUTING.md).
        for (const std::string_view rejected :
             {"1BAD", "9", ".dotfirst", "has-dash", "has space", "BAD!", "\xC5\xA1", "A#1"})
        {
            CHECK_MESSAGE(!engine.RegisterFunction(rejected, spec, constant),
                          "expected rejected: ", rejected);
        }

        // The accepted registrations evaluate through the parser as well.
        CHECK(H::Number(engine, "=MY.FUNC()") == doctest::Approx(1.0));
        CHECK(H::Number(engine, "=_PRIVATE()") == doctest::Approx(1.0));
    }

} // TEST_SUITE
