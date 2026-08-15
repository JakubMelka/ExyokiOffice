// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "FormulaParser.hpp"

#include "ExyokiOffice/Excel/ExcelAddress.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include "AsciiText.hpp"

#include <array>
#include <charconv>
#include <utility>

namespace ExyokiOffice::Excel
{

namespace FormulaParserHelpers
{

/** @brief Lexical token kinds produced by the formula lexer. */
enum class TokenKind
{
    End,
    Number,
    String,
    ErrorLiteral,
    /** Reference candidate or name, optionally sheet-qualified. */
    RefOrName,
    Plus,
    Minus,
    Star,
    Slash,
    Caret,
    Ampersand,
    Percent,
    Equal,
    NotEqual,
    Less,
    LessEqual,
    Greater,
    GreaterEqual,
    LeftParen,
    RightParen,
    LeftBrace,
    RightBrace,
    Comma,
    Semicolon,
    Colon,
    Bad
};

struct Token
{
    TokenKind kind = TokenKind::End;
    Size offset = 0;
    Size length = 0;
    Real number = 0.0;
    /** String literal value, error literal text, or reference body text. */
    std::string text;
    /** Sheet qualifier with quoting removed. */
    std::string sheet;
    bool hasSheet = false;
    bool external = false;
    bool precededByWhitespace = false;
};

bool IsAsciiLetter(char c)
{
    return AsciiText::IsAlpha(c);
}

bool IsAsciiDigit(char c)
{
    return AsciiText::IsDigit(c);
}

/// True for a byte that can appear inside a name; anything outside ASCII belongs to one.
bool IsIdentifierChar(char c)
{
    return AsciiText::IsAlnum(c) || AsciiText::IsNonAscii(c) || c == '_' || c == '.' || c == '$';
}

/** @brief Tokenizes canonical en-US formula text. */
class Lexer
{
public:
    explicit Lexer(std::string_view text)
        : m_text(text) {}

    Token Next()
    {
        const bool sawWhitespace = SkipWhitespace();
        Token token;
        token.precededByWhitespace = sawWhitespace;
        token.offset = m_position;

        if (m_position >= m_text.size())
        {
            token.kind = TokenKind::End;
            return token;
        }

        const char c = m_text[m_position];
        switch (c)
        {
            case '+':
                return SingleChar(token, TokenKind::Plus);
            case '-':
                return SingleChar(token, TokenKind::Minus);
            case '*':
                return SingleChar(token, TokenKind::Star);
            case '/':
                return SingleChar(token, TokenKind::Slash);
            case '^':
                return SingleChar(token, TokenKind::Caret);
            case '&':
                return SingleChar(token, TokenKind::Ampersand);
            case '%':
                return SingleChar(token, TokenKind::Percent);
            case '(':
                return SingleChar(token, TokenKind::LeftParen);
            case ')':
                return SingleChar(token, TokenKind::RightParen);
            case '{':
                return SingleChar(token, TokenKind::LeftBrace);
            case '}':
                return SingleChar(token, TokenKind::RightBrace);
            case ',':
                return SingleChar(token, TokenKind::Comma);
            case ';':
                return SingleChar(token, TokenKind::Semicolon);
            case ':':
                return SingleChar(token, TokenKind::Colon);
            case '=':
                return SingleChar(token, TokenKind::Equal);
            case '<':
                if (Peek(1) == '>')
                {
                    return MultiChar(token, TokenKind::NotEqual, 2);
                }
                if (Peek(1) == '=')
                {
                    return MultiChar(token, TokenKind::LessEqual, 2);
                }
                return SingleChar(token, TokenKind::Less);
            case '>':
                if (Peek(1) == '=')
                {
                    return MultiChar(token, TokenKind::GreaterEqual, 2);
                }
                return SingleChar(token, TokenKind::Greater);
            case '"':
                return LexString(token);
            case '#':
                return LexErrorLiteral(token);
            case '\'':
                return LexQuotedSheetReference(token);
            case '[':
                return LexExternalReference(token);
            default:
                break;
        }

        if (IsAsciiDigit(c) || (c == '.' && IsAsciiDigit(Peek(1))))
        {
            return LexNumber(token);
        }
        if (IsAsciiLetter(c) || c == '_' || c == '$' || static_cast<unsigned char>(c) >= 0x80)
        {
            return LexIdentifier(token);
        }

        token.kind = TokenKind::Bad;
        token.length = 1;
        token.text.assign(1, c);
        ++m_position;
        return token;
    }

private:
    bool SkipWhitespace()
    {
        bool sawWhitespace = false;
        while (m_position < m_text.size())
        {
            const char c = m_text[m_position];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
            {
                sawWhitespace = true;
                ++m_position;
            }
            else
            {
                break;
            }
        }
        return sawWhitespace;
    }

    char Peek(Size delta) const
    {
        const Size index = m_position + delta;
        return index < m_text.size() ? m_text[index] : '\0';
    }

    Token& Finish(Token& token)
    {
        token.length = m_position - token.offset;
        return token;
    }

    Token SingleChar(Token& token, TokenKind kind)
    {
        token.kind = kind;
        ++m_position;
        return Finish(token);
    }

    Token MultiChar(Token& token, TokenKind kind, Size length)
    {
        token.kind = kind;
        m_position += length;
        return Finish(token);
    }

    Token LexNumber(Token& token)
    {
        const Size start = m_position;
        while (IsAsciiDigit(Peek(0)))
        {
            ++m_position;
        }
        if (Peek(0) == '.')
        {
            ++m_position;
            while (IsAsciiDigit(Peek(0)))
            {
                ++m_position;
            }
        }
        if (Peek(0) == 'e' || Peek(0) == 'E')
        {
            Size exponent = 1;
            if (Peek(exponent) == '+' || Peek(exponent) == '-')
            {
                ++exponent;
            }
            if (IsAsciiDigit(Peek(exponent)))
            {
                m_position += exponent;
                while (IsAsciiDigit(Peek(0)))
                {
                    ++m_position;
                }
            }
        }

        const std::string_view numberText = m_text.substr(start, m_position - start);
        Real value = 0.0;
        const auto result = std::from_chars(numberText.data(), numberText.data() + numberText.size(), value);
        if (result.ec != std::errc() || result.ptr != numberText.data() + numberText.size())
        {
            token.kind = TokenKind::Bad;
            token.text = std::string(numberText);
            return Finish(token);
        }
        token.kind = TokenKind::Number;
        token.number = value;
        return Finish(token);
    }

    Token LexString(Token& token)
    {
        ++m_position; // opening quote
        std::string value;
        while (m_position < m_text.size())
        {
            const char c = m_text[m_position];
            if (c == '"')
            {
                if (Peek(1) == '"')
                {
                    value.push_back('"');
                    m_position += 2;
                    continue;
                }
                ++m_position; // closing quote
                token.kind = TokenKind::String;
                token.text = std::move(value);
                return Finish(token);
            }
            value.push_back(c);
            ++m_position;
        }
        token.kind = TokenKind::Bad;
        token.text = "unterminated string literal";
        return Finish(token);
    }

    Token LexErrorLiteral(Token& token)
    {
        static constexpr std::array<std::string_view, 7> literals = {
            "#NULL!", "#DIV/0!", "#VALUE!", "#REF!", "#NAME?", "#NUM!", "#N/A"};

        const std::string_view rest = m_text.substr(m_position);
        for (const std::string_view literal : literals)
        {
            if (rest.size() >= literal.size() &&
                AsciiText::EqualsIgnoreCase(rest.substr(0, literal.size()), literal))
            {
                token.kind = TokenKind::ErrorLiteral;
                token.text = std::string(literal);
                m_position += literal.size();
                return Finish(token);
            }
        }
        token.kind = TokenKind::Bad;
        token.text = "unknown error literal";
        ++m_position;
        return Finish(token);
    }

    Token LexQuotedSheetReference(Token& token)
    {
        ++m_position; // opening apostrophe
        std::string sheet;
        bool closed = false;
        while (m_position < m_text.size())
        {
            const char c = m_text[m_position];
            if (c == '\'')
            {
                if (Peek(1) == '\'')
                {
                    sheet.push_back('\'');
                    m_position += 2;
                    continue;
                }
                ++m_position;
                closed = true;
                break;
            }
            sheet.push_back(c);
            ++m_position;
        }
        if (!closed || Peek(0) != '!')
        {
            token.kind = TokenKind::Bad;
            token.text = "expected '!' after quoted sheet name";
            return Finish(token);
        }
        ++m_position; // '!'
        const bool external = sheet.find('[') != std::string::npos;
        return LexReferenceBody(token, std::move(sheet), external);
    }

    Token LexExternalReference(Token& token)
    {
        ++m_position; // '['
        std::string book;
        bool closed = false;
        while (m_position < m_text.size())
        {
            const char c = m_text[m_position];
            ++m_position;
            if (c == ']')
            {
                closed = true;
                break;
            }
            book.push_back(c);
        }
        if (!closed)
        {
            token.kind = TokenKind::Bad;
            token.text = "unterminated external workbook reference";
            return Finish(token);
        }
        std::string sheet = "[" + book + "]";
        while (m_position < m_text.size() && m_text[m_position] != '!')
        {
            sheet.push_back(m_text[m_position]);
            ++m_position;
        }
        if (Peek(0) != '!')
        {
            token.kind = TokenKind::Bad;
            token.text = "expected '!' after external sheet name";
            return Finish(token);
        }
        ++m_position; // '!'
        return LexReferenceBody(token, std::move(sheet), true);
    }

    Token LexIdentifier(Token& token)
    {
        const Size start = m_position;
        while (IsIdentifierChar(Peek(0)))
        {
            ++m_position;
        }
        std::string body(m_text.substr(start, m_position - start));

        if (Peek(0) == '!')
        {
            ++m_position;
            return LexReferenceBody(token, std::move(body), false);
        }

        // Unquoted 3-D sheet range such as Sheet1:Sheet2!A1. Look ahead for a
        // second identifier followed by '!'; otherwise restore the position so
        // ordinary range parsing sees the colon.
        if (Peek(0) == ':')
        {
            const Size savedPosition = m_position;
            ++m_position;
            const Size secondStart = m_position;
            while (IsIdentifierChar(Peek(0)))
            {
                ++m_position;
            }
            if (m_position > secondStart && Peek(0) == '!')
            {
                std::string sheet = body;
                sheet.push_back(':');
                sheet.append(m_text.substr(secondStart, m_position - secondStart));
                ++m_position; // '!'
                return LexReferenceBody(token, std::move(sheet), true);
            }
            m_position = savedPosition;
        }

        token.kind = TokenKind::RefOrName;
        token.text = std::move(body);
        return Finish(token);
    }

    Token LexReferenceBody(Token& token, std::string sheet, bool external)
    {
        token.sheet = std::move(sheet);
        token.hasSheet = true;
        token.external = external;

        if (Peek(0) == '#')
        {
            // A deleted sheet-qualified reference stored as Sheet1!#REF!.
            Token errorToken = LexErrorLiteral(token);
            errorToken.hasSheet = false;
            return errorToken;
        }

        const Size start = m_position;
        while (IsIdentifierChar(Peek(0)))
        {
            ++m_position;
        }
        if (m_position == start)
        {
            token.kind = TokenKind::Bad;
            token.text = "expected a cell reference after '!'";
            return Finish(token);
        }
        token.kind = TokenKind::RefOrName;
        token.text = std::string(m_text.substr(start, m_position - start));
        return Finish(token);
    }

    std::string_view m_text;
    Size m_position = 0;
};

/** @brief Classification of a reference body such as `$A$1` or `B`. */
struct ReferenceBody
{
    enum class Kind
    {
        None,
        Cell,
        Column,
        Row
    };

    Kind kind = Kind::None;
    FormulaCoordinate row;
    FormulaCoordinate column;
};

ReferenceBody ClassifyReferenceBody(std::string_view body)
{
    ReferenceBody result;
    Size position = 0;

    bool columnAbsolute = false;
    if (position < body.size() && body[position] == '$')
    {
        columnAbsolute = true;
        ++position;
    }
    Size letterStart = position;
    while (position < body.size() && IsAsciiLetter(body[position]))
    {
        ++position;
    }
    const std::string_view letters = body.substr(letterStart, position - letterStart);

    bool rowAbsolute = false;
    if (position < body.size() && body[position] == '$')
    {
        rowAbsolute = true;
        ++position;
    }
    Size digitStart = position;
    while (position < body.size() && IsAsciiDigit(body[position]))
    {
        ++position;
    }
    const std::string_view digits = body.substr(digitStart, position - digitStart);

    if (position != body.size())
    {
        return result;
    }

    std::optional<ColumnIndex> column;
    if (!letters.empty() && letters.size() <= 3)
    {
        column = ColumnIndex::ParseName(letters);
    }
    std::optional<UInt32> row;
    if (!digits.empty() && digits.size() <= 7)
    {
        UInt32 value = 0;
        const auto parsed = std::from_chars(digits.data(), digits.data() + digits.size(), value);
        if (parsed.ec == std::errc() && parsed.ptr == digits.data() + digits.size() &&
            value >= 1 && value <= MaxRowIndex)
        {
            row = value;
        }
    }

    if (!letters.empty() && !digits.empty())
    {
        if (column && row && !(rowAbsolute && digits.empty()))
        {
            result.kind = ReferenceBody::Kind::Cell;
            result.column = {column->Value(), columnAbsolute};
            result.row = {*row, rowAbsolute};
        }
        return result;
    }
    if (!letters.empty() && digits.empty() && !rowAbsolute)
    {
        if (column)
        {
            result.kind = ReferenceBody::Kind::Column;
            result.column = {column->Value(), columnAbsolute};
        }
        return result;
    }
    if (letters.empty() && !digits.empty() && !columnAbsolute)
    {
        if (row)
        {
            result.kind = ReferenceBody::Kind::Row;
            result.row = {*row, rowAbsolute};
        }
        return result;
    }
    return result;
}

/** @brief Recursive-descent parser over the token stream. */
class Parser
{
public:
    explicit Parser(std::string_view text)
        : m_lexer(text)
    {
        m_current = m_lexer.Next();
    }

    FormulaParseResult Run()
    {
        FormulaParseResult result;
        auto root = ParseExpression(0);
        if (!m_diagnostics.empty())
        {
            result.diagnostics = std::move(m_diagnostics);
            return result;
        }
        if (m_current.kind != TokenKind::End)
        {
            result.diagnostics.push_back(
                {m_current.offset, m_current.length, "Unexpected content after the end of the formula."});
            return result;
        }
        result.root = std::move(root);
        return result;
    }

private:
    using NodePtr = std::unique_ptr<FormulaExpression>;

    static constexpr int ComparisonPrecedence = 10;
    static constexpr int ConcatenationPrecedence = 20;
    static constexpr int AdditivePrecedence = 30;
    static constexpr int MultiplicativePrecedence = 40;
    static constexpr int PowerPrecedence = 50;
    static constexpr int IntersectionPrecedence = 80;

    void Advance()
    {
        m_current = m_lexer.Next();
    }

    void ReportError(const Token& token, std::string message)
    {
        if (m_diagnostics.empty())
        {
            m_diagnostics.push_back({token.offset, token.length, std::move(message)});
        }
    }

    bool HasError() const
    {
        return !m_diagnostics.empty();
    }

    NodePtr MakeNode(FormulaExpressionKind kind, Size offset, Size length)
    {
        auto node = std::make_unique<FormulaExpression>();
        node->kind = kind;
        node->offset = offset;
        node->length = length;
        return node;
    }

    static std::optional<FormulaBinaryOperator> BinaryOperatorFor(TokenKind kind)
    {
        switch (kind)
        {
            case TokenKind::Plus:
                return FormulaBinaryOperator::Add;
            case TokenKind::Minus:
                return FormulaBinaryOperator::Subtract;
            case TokenKind::Star:
                return FormulaBinaryOperator::Multiply;
            case TokenKind::Slash:
                return FormulaBinaryOperator::Divide;
            case TokenKind::Caret:
                return FormulaBinaryOperator::Power;
            case TokenKind::Ampersand:
                return FormulaBinaryOperator::Concatenate;
            case TokenKind::Equal:
                return FormulaBinaryOperator::Equal;
            case TokenKind::NotEqual:
                return FormulaBinaryOperator::NotEqual;
            case TokenKind::Less:
                return FormulaBinaryOperator::Less;
            case TokenKind::LessEqual:
                return FormulaBinaryOperator::LessEqual;
            case TokenKind::Greater:
                return FormulaBinaryOperator::Greater;
            case TokenKind::GreaterEqual:
                return FormulaBinaryOperator::GreaterEqual;
            default:
                return std::nullopt;
        }
    }

    static int PrecedenceFor(FormulaBinaryOperator op)
    {
        switch (op)
        {
            case FormulaBinaryOperator::Equal:
            case FormulaBinaryOperator::NotEqual:
            case FormulaBinaryOperator::Less:
            case FormulaBinaryOperator::LessEqual:
            case FormulaBinaryOperator::Greater:
            case FormulaBinaryOperator::GreaterEqual:
                return ComparisonPrecedence;
            case FormulaBinaryOperator::Concatenate:
                return ConcatenationPrecedence;
            case FormulaBinaryOperator::Add:
            case FormulaBinaryOperator::Subtract:
                return AdditivePrecedence;
            case FormulaBinaryOperator::Multiply:
            case FormulaBinaryOperator::Divide:
                return MultiplicativePrecedence;
            case FormulaBinaryOperator::Power:
                return PowerPrecedence;
            case FormulaBinaryOperator::Intersect:
                return IntersectionPrecedence;
        }
        return 0;
    }

    static bool CanStartReferenceOperand(const Token& token)
    {
        return token.kind == TokenKind::RefOrName || token.kind == TokenKind::LeftParen;
    }

    NodePtr ParseExpression(int minimumPrecedence)
    {
        NodePtr left = ParseUnary();
        if (HasError())
        {
            return nullptr;
        }

        while (true)
        {
            const auto binaryOp = BinaryOperatorFor(m_current.kind);
            if (binaryOp)
            {
                const int precedence = PrecedenceFor(*binaryOp);
                if (precedence < minimumPrecedence)
                {
                    break;
                }
                const Token operatorToken = m_current;
                Advance();
                NodePtr right = ParseExpression(precedence + 1);
                if (HasError())
                {
                    return nullptr;
                }
                auto node = MakeNode(FormulaExpressionKind::Binary, operatorToken.offset, operatorToken.length);
                node->binaryOperator = *binaryOp;
                node->children.push_back(std::move(left));
                node->children.push_back(std::move(right));
                left = std::move(node);
                continue;
            }

            // A space between two reference operands is the intersection
            // operator: `A1:B2 B1:C2`.
            if (m_current.precededByWhitespace && CanStartReferenceOperand(m_current) &&
                IntersectionPrecedence >= minimumPrecedence)
            {
                const Token operandToken = m_current;
                NodePtr right = ParseExpression(IntersectionPrecedence + 1);
                if (HasError())
                {
                    return nullptr;
                }
                auto node = MakeNode(FormulaExpressionKind::Binary, operandToken.offset, operandToken.length);
                node->binaryOperator = FormulaBinaryOperator::Intersect;
                node->children.push_back(std::move(left));
                node->children.push_back(std::move(right));
                left = std::move(node);
                continue;
            }
            break;
        }
        return left;
    }

    NodePtr ParseUnary()
    {
        if (m_current.kind == TokenKind::Plus || m_current.kind == TokenKind::Minus)
        {
            const Token operatorToken = m_current;
            Advance();
            NodePtr operand = ParseUnary();
            if (HasError())
            {
                return nullptr;
            }
            auto node = MakeNode(FormulaExpressionKind::Unary, operatorToken.offset, operatorToken.length);
            node->unaryOperator = operatorToken.kind == TokenKind::Minus ? FormulaUnaryOperator::Minus
                                                                         : FormulaUnaryOperator::Plus;
            node->children.push_back(std::move(operand));
            return node;
        }
        return ParsePostfix();
    }

    NodePtr ParsePostfix()
    {
        NodePtr operand = ParsePrimary();
        if (HasError())
        {
            return nullptr;
        }
        while (m_current.kind == TokenKind::Percent)
        {
            const Token operatorToken = m_current;
            Advance();
            auto node = MakeNode(FormulaExpressionKind::Unary, operatorToken.offset, operatorToken.length);
            node->unaryOperator = FormulaUnaryOperator::Percent;
            node->children.push_back(std::move(operand));
            operand = std::move(node);
        }
        return operand;
    }

    NodePtr ParsePrimary()
    {
        switch (m_current.kind)
        {
            case TokenKind::Number:
                return ParseNumberOrWholeRowRange();
            case TokenKind::String:
                return ParseStringLiteral();
            case TokenKind::ErrorLiteral:
                return ParseErrorLiteral();
            case TokenKind::RefOrName:
                return ParseReferenceOrName();
            case TokenKind::LeftParen:
                return ParseParenthesized();
            case TokenKind::LeftBrace:
                return ParseArrayLiteral();
            case TokenKind::End:
                ReportError(m_current, "Unexpected end of formula; an operand was expected.");
                return nullptr;
            case TokenKind::Bad:
                ReportError(m_current, m_current.text.empty()
                                           ? std::string("Unrecognized token.")
                                           : "Unrecognized token: " + m_current.text + ".");
                return nullptr;
            default:
                ReportError(m_current, "An operand was expected here.");
                return nullptr;
        }
    }

    NodePtr ParseNumberOrWholeRowRange()
    {
        const Token numberToken = m_current;
        Advance();

        // `1:3` denotes a whole-row range when both endpoints are integral
        // row numbers inside the grid.
        if (m_current.kind == TokenKind::Colon && IsRowNumber(numberToken))
        {
            Advance();
            if (m_current.kind != TokenKind::Number || !IsRowNumber(m_current))
            {
                ReportError(m_current, "Expected a row number after ':' in a whole-row reference.");
                return nullptr;
            }
            const Token secondToken = m_current;
            Advance();

            auto node = MakeNode(FormulaExpressionKind::Reference, numberToken.offset,
                                 secondToken.offset + secondToken.length - numberToken.offset);
            auto firstRow = static_cast<UInt32>(numberToken.number);
            auto lastRow = static_cast<UInt32>(secondToken.number);
            if (firstRow > lastRow)
            {
                std::swap(firstRow, lastRow);
            }
            node->area.firstRow = {firstRow, false};
            node->area.lastRow = {lastRow, false};
            node->area.firstColumn = {1, true};
            node->area.lastColumn = {MaxColumnIndex, true};
            node->area.wholeRows = true;
            return node;
        }

        auto node = MakeNode(FormulaExpressionKind::NumberLiteral, numberToken.offset, numberToken.length);
        node->number = numberToken.number;
        return node;
    }

    static bool IsRowNumber(const Token& token)
    {
        if (token.kind != TokenKind::Number)
        {
            return false;
        }
        const Real value = token.number;
        return value >= 1.0 && value <= static_cast<Real>(MaxRowIndex) &&
               value == static_cast<Real>(static_cast<UInt32>(value));
    }

    NodePtr ParseStringLiteral()
    {
        auto node = MakeNode(FormulaExpressionKind::StringLiteral, m_current.offset, m_current.length);
        node->text = m_current.text;
        Advance();
        return node;
    }

    NodePtr ParseErrorLiteral()
    {
        auto node = MakeNode(FormulaExpressionKind::ErrorLiteral, m_current.offset, m_current.length);
        const auto code = ParseFormulaErrorText(m_current.text);
        node->error = code.value_or(FormulaErrorCode::Value);
        Advance();
        return node;
    }

    NodePtr ParseReferenceOrName()
    {
        const Token firstToken = m_current;
        Advance();

        if (!firstToken.hasSheet && m_current.kind != TokenKind::LeftParen)
        {
            if (AsciiText::EqualsIgnoreCase(firstToken.text, "TRUE") || AsciiText::EqualsIgnoreCase(firstToken.text, "FALSE"))
            {
                auto node = MakeNode(FormulaExpressionKind::BooleanLiteral, firstToken.offset, firstToken.length);
                node->boolean = AsciiText::EqualsIgnoreCase(firstToken.text, "TRUE");
                return node;
            }
        }

        if (m_current.kind == TokenKind::LeftParen && !firstToken.hasSheet)
        {
            return ParseFunctionCall(firstToken);
        }

        const ReferenceBody firstBody = ClassifyReferenceBody(firstToken.text);

        if (firstBody.kind == ReferenceBody::Kind::Cell)
        {
            auto node = MakeNode(FormulaExpressionKind::Reference, firstToken.offset, firstToken.length);
            node->area.firstRow = firstBody.row;
            node->area.lastRow = firstBody.row;
            node->area.firstColumn = firstBody.column;
            node->area.lastColumn = firstBody.column;
            node->area.sheet = firstToken.sheet;
            node->area.hasSheet = firstToken.hasSheet;
            node->area.external = firstToken.external;

            if (m_current.kind == TokenKind::Colon)
            {
                return ParseRangeTail(std::move(node), firstToken);
            }
            NormalizeArea(node->area);
            return node;
        }

        if (firstBody.kind == ReferenceBody::Kind::Column && m_current.kind == TokenKind::Colon)
        {
            Advance();
            if (m_current.kind != TokenKind::RefOrName || m_current.hasSheet)
            {
                ReportError(m_current, "Expected a column letter after ':' in a whole-column reference.");
                return nullptr;
            }
            const ReferenceBody secondBody = ClassifyReferenceBody(m_current.text);
            if (secondBody.kind != ReferenceBody::Kind::Column)
            {
                ReportError(m_current, "Expected a column letter after ':' in a whole-column reference.");
                return nullptr;
            }
            const Token secondToken = m_current;
            Advance();

            auto node = MakeNode(FormulaExpressionKind::Reference, firstToken.offset,
                                 secondToken.offset + secondToken.length - firstToken.offset);
            node->area.firstColumn = firstBody.column;
            node->area.lastColumn = secondBody.column;
            if (node->area.firstColumn.value > node->area.lastColumn.value)
            {
                std::swap(node->area.firstColumn, node->area.lastColumn);
            }
            node->area.firstRow = {1, true};
            node->area.lastRow = {MaxRowIndex, true};
            node->area.wholeColumns = true;
            node->area.sheet = firstToken.sheet;
            node->area.hasSheet = firstToken.hasSheet;
            node->area.external = firstToken.external;
            return node;
        }

        // Anything else is a defined name or a structured reference. The
        // sheet qualifier, when present, selects the name's scope sheet.
        auto node = MakeNode(FormulaExpressionKind::NameReference, firstToken.offset, firstToken.length);
        node->text = firstToken.text;
        node->area.hasSheet = firstToken.hasSheet;
        node->area.sheet = firstToken.sheet;
        if (firstToken.external)
        {
            node->kind = FormulaExpressionKind::Reference;
            node->area.external = true;
        }
        return node;
    }

    NodePtr ParseRangeTail(NodePtr firstNode, const Token& firstToken)
    {
        Advance(); // ':'
        if (m_current.kind != TokenKind::RefOrName)
        {
            ReportError(m_current, "Expected a cell reference after ':' in a range.");
            return nullptr;
        }
        const ReferenceBody secondBody = ClassifyReferenceBody(m_current.text);
        if (secondBody.kind != ReferenceBody::Kind::Cell)
        {
            ReportError(m_current, "Expected a cell reference after ':' in a range.");
            return nullptr;
        }
        if (m_current.hasSheet)
        {
            ReportError(m_current, "A range endpoint must not repeat the sheet qualifier.");
            return nullptr;
        }
        const Token secondToken = m_current;
        Advance();

        firstNode->length = secondToken.offset + secondToken.length - firstToken.offset;
        firstNode->area.lastRow = secondBody.row;
        firstNode->area.lastColumn = secondBody.column;
        NormalizeArea(firstNode->area);
        return firstNode;
    }

    static void NormalizeArea(FormulaReferenceArea& area)
    {
        if (area.firstRow.value > area.lastRow.value)
        {
            std::swap(area.firstRow, area.lastRow);
        }
        if (area.firstColumn.value > area.lastColumn.value)
        {
            std::swap(area.firstColumn, area.lastColumn);
        }
    }

    NodePtr ParseFunctionCall(const Token& nameToken)
    {
        auto node = MakeNode(FormulaExpressionKind::FunctionCall, nameToken.offset, nameToken.length);
        std::string name = nameToken.text;
        static constexpr std::string_view xlfnPrefix = "_xlfn.";
        if (name.size() > xlfnPrefix.size() &&
            AsciiText::EqualsIgnoreCase(std::string_view(name).substr(0, xlfnPrefix.size()), xlfnPrefix))
        {
            name.erase(0, xlfnPrefix.size());
        }
        for (char& c : name)
        {
            c = AsciiText::ToUpper(c);
        }
        node->text = std::move(name);

        Advance(); // '('
        if (m_current.kind == TokenKind::RightParen)
        {
            Advance();
            return node;
        }

        while (true)
        {
            if (m_current.kind == TokenKind::Comma || m_current.kind == TokenKind::RightParen)
            {
                node->children.push_back(
                    MakeNode(FormulaExpressionKind::EmptyArgument, m_current.offset, 0));
            }
            else
            {
                NodePtr argument = ParseExpression(0);
                if (HasError())
                {
                    return nullptr;
                }
                node->children.push_back(std::move(argument));
            }

            if (m_current.kind == TokenKind::Comma)
            {
                Advance();
                continue;
            }
            if (m_current.kind == TokenKind::RightParen)
            {
                Advance();
                return node;
            }
            ReportError(m_current, "Expected ',' or ')' in the function argument list.");
            return nullptr;
        }
    }

    NodePtr ParseParenthesized()
    {
        const Token openToken = m_current;
        Advance(); // '('
        NodePtr first = ParseExpression(0);
        if (HasError())
        {
            return nullptr;
        }

        if (m_current.kind == TokenKind::Comma)
        {
            // `(A1,B2)` is the reference union operator.
            auto node = MakeNode(FormulaExpressionKind::Union, openToken.offset, openToken.length);
            node->children.push_back(std::move(first));
            while (m_current.kind == TokenKind::Comma)
            {
                Advance();
                NodePtr operand = ParseExpression(0);
                if (HasError())
                {
                    return nullptr;
                }
                node->children.push_back(std::move(operand));
            }
            if (m_current.kind != TokenKind::RightParen)
            {
                ReportError(m_current, "Expected ')' to close the reference union.");
                return nullptr;
            }
            node->length = m_current.offset + m_current.length - openToken.offset;
            Advance();
            return node;
        }

        if (m_current.kind != TokenKind::RightParen)
        {
            ReportError(m_current, "Expected ')' to close the parenthesized expression.");
            return nullptr;
        }
        Advance();
        return first;
    }

    NodePtr ParseArrayLiteral()
    {
        const Token openToken = m_current;
        Advance(); // '{'

        auto node = MakeNode(FormulaExpressionKind::ArrayLiteral, openToken.offset, openToken.length);
        Size columnCount = 0;
        Size currentRowColumns = 0;
        Size rowCount = 1;

        while (true)
        {
            NodePtr element = ParseArrayElement();
            if (HasError())
            {
                return nullptr;
            }
            node->children.push_back(std::move(element));
            ++currentRowColumns;

            if (m_current.kind == TokenKind::Comma)
            {
                Advance();
                continue;
            }
            if (m_current.kind == TokenKind::Semicolon)
            {
                if (columnCount == 0)
                {
                    columnCount = currentRowColumns;
                }
                else if (currentRowColumns != columnCount)
                {
                    ReportError(m_current, "Array rows must all have the same number of columns.");
                    return nullptr;
                }
                currentRowColumns = 0;
                ++rowCount;
                Advance();
                continue;
            }
            if (m_current.kind == TokenKind::RightBrace)
            {
                if (columnCount == 0)
                {
                    columnCount = currentRowColumns;
                }
                else if (currentRowColumns != columnCount)
                {
                    ReportError(m_current, "Array rows must all have the same number of columns.");
                    return nullptr;
                }
                node->length = m_current.offset + m_current.length - openToken.offset;
                Advance();
                node->arrayRowCount = rowCount;
                node->arrayColumnCount = columnCount;
                return node;
            }
            ReportError(m_current, "Expected ',', ';', or '}' in the array constant.");
            return nullptr;
        }
    }

    NodePtr ParseArrayElement()
    {
        bool negate = false;
        Token elementToken = m_current;
        if (m_current.kind == TokenKind::Minus || m_current.kind == TokenKind::Plus)
        {
            negate = m_current.kind == TokenKind::Minus;
            Advance();
        }

        switch (m_current.kind)
        {
            case TokenKind::Number:
            {
                auto node = MakeNode(FormulaExpressionKind::NumberLiteral, elementToken.offset,
                                     m_current.offset + m_current.length - elementToken.offset);
                node->number = negate ? -m_current.number : m_current.number;
                Advance();
                return node;
            }
            case TokenKind::String:
            {
                if (negate)
                {
                    break;
                }
                return ParseStringLiteral();
            }
            case TokenKind::ErrorLiteral:
            {
                if (negate)
                {
                    break;
                }
                return ParseErrorLiteral();
            }
            case TokenKind::RefOrName:
            {
                if (!negate && !m_current.hasSheet &&
                    (AsciiText::EqualsIgnoreCase(m_current.text, "TRUE") || AsciiText::EqualsIgnoreCase(m_current.text, "FALSE")))
                {
                    auto node = MakeNode(FormulaExpressionKind::BooleanLiteral, m_current.offset, m_current.length);
                    node->boolean = AsciiText::EqualsIgnoreCase(m_current.text, "TRUE");
                    Advance();
                    return node;
                }
                break;
            }
            default:
                break;
        }

        ReportError(m_current, "Array constants may contain only numbers, text, logical values, and errors.");
        return nullptr;
    }

    Lexer m_lexer;
    Token m_current;
    std::vector<FormulaDiagnostic> m_diagnostics;
};

} // namespace FormulaParserHelpers

FormulaParseResult FormulaParser::Parse(std::string_view formula)
{
    if (!formula.empty() && formula.front() == '=')
    {
        formula.remove_prefix(1);
    }
    if (formula.empty())
    {
        FormulaParseResult result;
        result.diagnostics.push_back({0, 0, "The formula is empty."});
        return result;
    }
    FormulaParserHelpers::Parser parser(formula);
    return parser.Run();
}

} // namespace ExyokiOffice::Excel
