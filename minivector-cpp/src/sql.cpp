#include "sql.h"
#include "text_scan.h"

#include <cctype>
#include <cstdlib>
#include <limits>

namespace {

enum class TK {
    Ident,
    Str,
    Num,
    LParen,
    RParen,
    Comma,
    Semi,
    Star,
    LBracket,
    RBracket,
    Eq,
    Ne,
    Lt,
    Le,
    Gt,
    Ge,
    Minus,
    Plus,
    At,
    End,
};

struct Token {
    TK kind;
    std::string text;
};

const char* token_name(TK kind) {
    switch (kind) {
        case TK::Ident:
            return "identifier";
        case TK::Str:
            return "string";
        case TK::Num:
            return "number";
        case TK::LParen:
            return "'('";
        case TK::RParen:
            return "')'";
        case TK::Comma:
            return "','";
        case TK::Semi:
            return "';'";
        case TK::Star:
            return "'*'";
        case TK::LBracket:
            return "'['";
        case TK::RBracket:
            return "']'";
        case TK::Eq:
            return "'='";
        case TK::Ne:
            return "'<>'";
        case TK::Lt:
            return "'<'";
        case TK::Le:
            return "'<='";
        case TK::Gt:
            return "'>'";
        case TK::Ge:
            return "'>='";
        case TK::Minus:
            return "'-'";
        case TK::Plus:
            return "'+'";
        case TK::At:
            return "'@'";
        case TK::End:
            return "end of statement";
    }
    return "token";
}

std::vector<Token> tokenize(const std::string& input) {
    std::vector<Token> tokens;
    size_t index = 0;
    while (index < input.size()) {
        unsigned char ch = static_cast<unsigned char>(input[index]);
        if (std::isspace(ch)) {
            ++index;
            continue;
        }
        if (ch == '-' && index + 1 < input.size() && input[index + 1] == '-') {
            while (index < input.size() && input[index] != '\n') {
                ++index;
            }
            continue;
        }
        if (ch == '#') {
            while (index < input.size() && input[index] != '\n') {
                ++index;
            }
            continue;
        }
        if (ch == '/' && index + 1 < input.size() && input[index + 1] == '*') {
            index += 2;
            while (index + 1 < input.size() &&
                   !(input[index] == '*' && input[index + 1] == '/')) {
                ++index;
            }
            if (index + 1 >= input.size()) {
                fail_syntax("unterminated block comment");
            }
            index += 2;
            continue;
        }
        if (ch == '\'' || ch == '"') {
            char quote = static_cast<char>(ch);
            ++index;
            std::string text;
            bool closed = false;
            while (index < input.size()) {
                const size_t span = plain_text_span(input.data() + index, input.size() - index, quote, false);
                text.append(input.data() + index, span);
                index += span;
                if (index == input.size()) break;
                char current = input[index];
                if (current == '\\' && index + 1 < input.size()) {
                    char escaped = input[index + 1];
                    switch (escaped) {
                        case 'n':
                            text.push_back('\n');
                            break;
                        case 't':
                            text.push_back('\t');
                            break;
                        case 'r':
                            text.push_back('\r');
                            break;
                        case '0':
                            text.push_back('\0');
                            break;
                        case '\\':
                            text.push_back('\\');
                            break;
                        case '\'':
                            text.push_back('\'');
                            break;
                        case '"':
                            text.push_back('"');
                            break;
                        default:
                            text.push_back('\\');
                            text.push_back(escaped);
                            break;
                    }
                    index += 2;
                    continue;
                }
                if (current == quote) {
                    if (index + 1 < input.size() && input[index + 1] == quote) {
                        text.push_back(quote);
                        index += 2;
                        continue;
                    }
                    ++index;
                    closed = true;
                    break;
                }
                text.push_back(current);
                ++index;
            }
            if (!closed) {
                fail_syntax("unterminated string literal");
            }
            tokens.push_back(Token{TK::Str, std::move(text)});
            continue;
        }
        if (ch == '`') {
            ++index;
            std::string text;
            bool closed = false;
            while (index < input.size()) {
                char current = input[index];
                if (current == '`') {
                    if (index + 1 < input.size() && input[index + 1] == '`') {
                        text.push_back('`');
                        index += 2;
                        continue;
                    }
                    ++index;
                    closed = true;
                    break;
                }
                text.push_back(current);
                ++index;
            }
            if (!closed) {
                fail_syntax("unterminated quoted identifier");
            }
            tokens.push_back(Token{TK::Ident, std::move(text)});
            continue;
        }
        if (std::isdigit(ch) ||
            (ch == '.' && index + 1 < input.size() &&
             std::isdigit(static_cast<unsigned char>(input[index + 1])))) {
            size_t start = index;
            bool seen_dot = false;
            bool seen_exp = false;
            while (index < input.size()) {
                char current = input[index];
                if (std::isdigit(static_cast<unsigned char>(current))) {
                    ++index;
                } else if (current == '.' && !seen_dot && !seen_exp) {
                    seen_dot = true;
                    ++index;
                } else if ((current == 'e' || current == 'E') && !seen_exp) {
                    seen_exp = true;
                    ++index;
                    if (index < input.size() &&
                        (input[index] == '+' || input[index] == '-')) {
                        ++index;
                    }
                } else {
                    break;
                }
            }
            tokens.push_back(Token{TK::Num, input.substr(start, index - start)});
            continue;
        }
        if (std::isalpha(ch) || ch == '_' || ch == '$') {
            size_t start = index;
            while (index < input.size()) {
                unsigned char current = static_cast<unsigned char>(input[index]);
                if (std::isalnum(current) || current == '_' || current == '$') {
                    ++index;
                } else {
                    break;
                }
            }
            tokens.push_back(Token{TK::Ident, input.substr(start, index - start)});
            continue;
        }
        switch (ch) {
            case '(':
                tokens.push_back(Token{TK::LParen, "("});
                ++index;
                break;
            case ')':
                tokens.push_back(Token{TK::RParen, ")"});
                ++index;
                break;
            case ',':
                tokens.push_back(Token{TK::Comma, ","});
                ++index;
                break;
            case ';':
                tokens.push_back(Token{TK::Semi, ";"});
                ++index;
                break;
            case '*':
                tokens.push_back(Token{TK::Star, "*"});
                ++index;
                break;
            case '[':
                tokens.push_back(Token{TK::LBracket, "["});
                ++index;
                break;
            case ']':
                tokens.push_back(Token{TK::RBracket, "]"});
                ++index;
                break;
            case '=':
                tokens.push_back(Token{TK::Eq, "="});
                ++index;
                if (index < input.size() && input[index] == '=') {
                    ++index;
                }
                break;
            case '!':
                if (index + 1 < input.size() && input[index + 1] == '=') {
                    tokens.push_back(Token{TK::Ne, "<>"});
                    index += 2;
                } else {
                    fail_syntax("unexpected character '!'");
                }
                break;
            case '<':
                if (index + 1 < input.size() && input[index + 1] == '=') {
                    tokens.push_back(Token{TK::Le, "<="});
                    index += 2;
                } else if (index + 1 < input.size() && input[index + 1] == '>') {
                    tokens.push_back(Token{TK::Ne, "<>"});
                    index += 2;
                } else {
                    tokens.push_back(Token{TK::Lt, "<"});
                    ++index;
                }
                break;
            case '>':
                if (index + 1 < input.size() && input[index + 1] == '=') {
                    tokens.push_back(Token{TK::Ge, ">="});
                    index += 2;
                } else {
                    tokens.push_back(Token{TK::Gt, ">"});
                    ++index;
                }
                break;
            case '-':
                tokens.push_back(Token{TK::Minus, "-"});
                ++index;
                break;
            case '+':
                tokens.push_back(Token{TK::Plus, "+"});
                ++index;
                break;
            case '@':
                tokens.push_back(Token{TK::At, "@"});
                ++index;
                break;
            default:
                fail_syntax("unexpected character '" + std::string(1, static_cast<char>(ch)) + "'");
        }
    }
    return tokens;
}

class Parser {
public:
    explicit Parser(std::vector<Token> tokens) : tokens_(std::move(tokens)) {}

    Stmt parse() {
        Stmt statement = parse_statement();
        while (is(TK::Semi)) {
            next();
        }
        if (peek().kind != TK::End) {
            fail_syntax("unexpected token near '" + describe() + "'");
        }
        return statement;
    }

private:
    std::vector<Token> tokens_;
    size_t pos_ = 0;

    const Token& peek() const {
        static const Token end_token{TK::End, ""};
        return pos_ < tokens_.size() ? tokens_[pos_] : end_token;
    }

    bool is(TK kind) const { return peek().kind == kind; }

    Token next() {
        if (pos_ >= tokens_.size()) {
            fail_syntax("unexpected end of statement");
        }
        return tokens_[pos_++];
    }

    std::string describe() const {
        if (pos_ >= tokens_.size()) {
            return "end of statement";
        }
        const Token& token = tokens_[pos_];
        if (token.kind == TK::Ident) {
            return token.text;
        }
        if (token.kind == TK::Str) {
            return "'" + token.text + "'";
        }
        if (token.kind == TK::Num) {
            return token.text;
        }
        return token_name(token.kind);
    }

    bool is_kw(std::string_view keyword) const {
        return peek().kind == TK::Ident && iequals(peek().text, keyword);
    }

    bool eat_kw(std::string_view keyword) {
        if (is_kw(keyword)) {
            ++pos_;
            return true;
        }
        return false;
    }

    void expect_kw(std::string_view keyword) {
        if (!eat_kw(keyword)) {
            fail_syntax("expected " + std::string(keyword) + ", found '" + describe() + "'");
        }
    }

    std::string parse_ident() {
        if (peek().kind != TK::Ident) {
            fail_syntax("expected identifier, found '" + describe() + "'");
        }
        return next().text;
    }

    void expect(TK kind) {
        if (!is(kind)) {
            fail_syntax(
                std::string("expected ") + token_name(kind) + ", found '" + describe() + "'");
        }
        ++pos_;
    }

    Stmt parse_statement() {
        if (is_kw("SELECT")) {
            return parse_select();
        }
        if (is_kw("INSERT")) {
            return parse_insert();
        }
        if (is_kw("UPDATE")) {
            return parse_update();
        }
        if (is_kw("DELETE")) {
            return parse_delete();
        }
        if (is_kw("CREATE")) {
            return parse_create();
        }
        if (eat_kw("BEGIN")) {
            eat_kw("WORK");
            Stmt stmt;
            stmt.kind = StmtKind::Begin;
            return stmt;
        }
        if (eat_kw("START")) {
            expect_kw("TRANSACTION");
            Stmt stmt;
            stmt.kind = StmtKind::Begin;
            return stmt;
        }
        if (eat_kw("COMMIT")) {
            eat_kw("WORK");
            Stmt stmt;
            stmt.kind = StmtKind::Commit;
            return stmt;
        }
        if (eat_kw("ROLLBACK")) {
            eat_kw("WORK");
            Stmt stmt;
            stmt.kind = StmtKind::Rollback;
            return stmt;
        }
        if (is_kw("SET")) {
            return parse_set();
        }
        if (is_kw("USE")) {
            while (peek().kind != TK::End) {
                next();
            }
            Stmt stmt;
            stmt.kind = StmtKind::Noop;
            return stmt;
        }
        throw DbError(
            1235, "42000", "statement '" + describe() + "' is not supported");
    }

    Stmt parse_create() {
        expect_kw("CREATE");
        if (eat_kw("TABLE")) {
            return parse_create_table();
        }
        if (eat_kw("VECTOR")) {
            expect_kw("INDEX");
            return parse_create_index();
        }
        if (eat_kw("INDEX")) {
            return parse_create_index();
        }
        fail_syntax("unsupported CREATE object '" + describe() + "'");
    }

    Stmt parse_create_table() {
        Stmt stmt;
        stmt.kind = StmtKind::CreateTable;
        if (eat_kw("IF")) {
            expect_kw("NOT");
            expect_kw("EXISTS");
            stmt.if_not_exists = true;
        }
        stmt.table = parse_ident();
        expect(TK::LParen);
        while (true) {
            if (eat_kw("PRIMARY")) {
                expect_kw("KEY");
                expect(TK::LParen);
                std::string pk_name = parse_ident();
                expect(TK::RParen);
                if (stmt.table_pk.has_value()) {
                    throw DbError(1068, "42000", "Multiple primary key defined");
                }
                stmt.table_pk = pk_name;
            } else {
                ColumnDef column;
                column.name = parse_ident();
                std::string type_name = parse_ident();
                if (iequals(type_name, "BIGINT")) {
                    column.type = ColumnType::bigint();
                } else if (iequals(type_name, "VARCHAR")) {
                    expect(TK::LParen);
                    uint64_t length = parse_unsigned_int();
                    expect(TK::RParen);
                    if (length == 0 || length > std::numeric_limits<uint32_t>::max()) {
                        fail_syntax("VARCHAR length is out of range");
                    }
                    column.type = ColumnType::varchar(static_cast<uint32_t>(length));
                } else if (iequals(type_name, "VECTOR")) {
                    expect(TK::LParen);
                    uint64_t dim = parse_unsigned_int();
                    expect(TK::RParen);
                    if (dim == 0 || dim > std::numeric_limits<uint32_t>::max()) {
                        fail_syntax("VECTOR dimension is out of range");
                    }
                    column.type = ColumnType::vector(static_cast<uint32_t>(dim));
                } else {
                    fail_syntax("unsupported column type '" + type_name + "'");
                }
                if (eat_kw("PRIMARY")) {
                    expect_kw("KEY");
                    column.primary = true;
                }
                stmt.columns.push_back(std::move(column));
            }
            if (is(TK::Comma)) {
                next();
                continue;
            }
            break;
        }
        expect(TK::RParen);
        if (stmt.columns.empty()) {
            fail_syntax("a table needs at least one column");
        }
        return stmt;
    }

    Stmt parse_create_index() {
        Stmt stmt;
        stmt.kind = StmtKind::CreateIndex;
        if (eat_kw("IF")) {
            expect_kw("NOT");
            expect_kw("EXISTS");
            stmt.if_not_exists = true;
        }
        stmt.name = parse_ident();
        expect_kw("ON");
        stmt.table = parse_ident();
        expect(TK::LParen);
        stmt.column = parse_ident();
        expect(TK::RParen);
        return stmt;
    }

    Stmt parse_insert() {
        Stmt stmt;
        stmt.kind = StmtKind::Insert;
        expect_kw("INSERT");
        expect_kw("INTO");
        stmt.table = parse_ident();
        if (is(TK::LParen)) {
            next();
            std::vector<std::string> columns;
            while (true) {
                columns.push_back(parse_ident());
                if (is(TK::Comma)) {
                    next();
                    continue;
                }
                break;
            }
            expect(TK::RParen);
            stmt.insert_columns = std::move(columns);
        }
        expect_kw("VALUES");
        while (true) {
            expect(TK::LParen);
            std::vector<Literal> values;
            while (true) {
                values.push_back(parse_literal());
                if (is(TK::Comma)) {
                    next();
                    continue;
                }
                break;
            }
            expect(TK::RParen);
            stmt.rows.push_back(std::move(values));
            if (is(TK::Comma)) {
                next();
                continue;
            }
            break;
        }
        return stmt;
    }

    Stmt parse_select() {
        Stmt stmt;
        stmt.kind = StmtKind::Select;
        expect_kw("SELECT");
        while (true) {
            if (is(TK::Star)) {
                next();
                SelectItem item;
                item.kind = SelectItemKind::Star;
                stmt.items.push_back(std::move(item));
            } else if (is_kw("L2_DISTANCE")) {
                next();
                expect(TK::LParen);
                SelectItem item;
                item.kind = SelectItemKind::Distance;
                item.column = parse_ident();
                expect(TK::Comma);
                item.query = parse_vector_argument();
                expect(TK::RParen);
                if (eat_kw("AS")) {
                    item.alias = parse_ident();
                }
                stmt.items.push_back(std::move(item));
            } else {
                SelectItem item;
                item.kind = SelectItemKind::Column;
                item.name = parse_ident();
                if (eat_kw("AS")) {
                    item.alias = parse_ident();
                }
                stmt.items.push_back(std::move(item));
            }
            if (is(TK::Comma)) {
                next();
                continue;
            }
            break;
        }
        expect_kw("FROM");
        stmt.table = parse_ident();
        if (eat_kw("WHERE")) {
            stmt.filter = parse_cond();
        }
        if (eat_kw("ORDER")) {
            expect_kw("BY");
            stmt.orders = parse_orders();
        }
        if (eat_kw("LIMIT")) {
            stmt.limit = parse_unsigned_int();
        }
        return stmt;
    }

    std::vector<OrderBy> parse_orders() {
        std::vector<OrderBy> orders;
        while (true) {
            orders.push_back(parse_order());
            if (!is(TK::Comma)) {
                break;
            }
            next();
        }
        return orders;
    }

    OrderBy parse_order() {
        OrderBy order;
        if (is_kw("L2_DISTANCE")) {
            next();
            order.kind = OrderKind::Distance;
            expect(TK::LParen);
            order.column = parse_ident();
            expect(TK::Comma);
            order.query = parse_vector_argument();
            expect(TK::RParen);
        } else {
            order.kind = OrderKind::Name;
            order.name = parse_ident();
        }
        eat_kw("APPROXIMATE");
        eat_kw("EXACT");
        if (eat_kw("ASC")) {
            order.asc = true;
        } else if (eat_kw("DESC")) {
            order.asc = false;
        }
        eat_kw("APPROXIMATE");
        eat_kw("EXACT");
        return order;
    }

    Stmt parse_update() {
        Stmt stmt;
        stmt.kind = StmtKind::Update;
        expect_kw("UPDATE");
        stmt.table = parse_ident();
        expect_kw("SET");
        while (true) {
            std::string column = parse_ident();
            expect(TK::Eq);
            Literal value = parse_literal();
            stmt.sets.emplace_back(std::move(column), std::move(value));
            if (is(TK::Comma)) {
                next();
                continue;
            }
            break;
        }
        if (eat_kw("WHERE")) {
            stmt.filter = parse_cond();
        }
        return stmt;
    }

    Stmt parse_delete() {
        Stmt stmt;
        stmt.kind = StmtKind::Delete;
        expect_kw("DELETE");
        expect_kw("FROM");
        stmt.table = parse_ident();
        if (eat_kw("WHERE")) {
            stmt.filter = parse_cond();
        }
        return stmt;
    }

    Cond parse_cond() {
        Cond cond;
        while (true) {
            if (is(TK::LParen)) {
                next();
                Cond inner = parse_cond();
                expect(TK::RParen);
                for (Clause& clause : inner.clauses) {
                    cond.clauses.push_back(std::move(clause));
                }
            } else {
                bool left_is_column = peek().kind == TK::Ident;
                std::string column;
                Literal literal;
                if (left_is_column) {
                    column = parse_ident();
                } else {
                    literal = parse_literal();
                }
                CmpOp op = parse_cmp_op();
                bool right_is_column = peek().kind == TK::Ident;
                if (left_is_column && !right_is_column) {
                    Clause clause;
                    clause.column = std::move(column);
                    clause.op = op;
                    clause.value = parse_literal();
                    cond.clauses.push_back(std::move(clause));
                } else if (!left_is_column && right_is_column) {
                    Clause clause;
                    clause.column = parse_ident();
                    clause.op = invert(op);
                    clause.value = std::move(literal);
                    cond.clauses.push_back(std::move(clause));
                } else {
                    fail_syntax("comparison needs one column and one literal");
                }
            }
            if (eat_kw("AND")) {
                continue;
            }
            break;
        }
        return cond;
    }

    CmpOp parse_cmp_op() {
        switch (next().kind) {
            case TK::Eq:
                return CmpOp::Eq;
            case TK::Ne:
                return CmpOp::Ne;
            case TK::Lt:
                return CmpOp::Lt;
            case TK::Le:
                return CmpOp::Le;
            case TK::Gt:
                return CmpOp::Gt;
            case TK::Ge:
                return CmpOp::Ge;
            default:
                fail_syntax("expected comparison operator, found '" + describe() + "'");
        }
    }

    static CmpOp invert(CmpOp op) {
        switch (op) {
            case CmpOp::Eq:
                return CmpOp::Eq;
            case CmpOp::Ne:
                return CmpOp::Ne;
            case CmpOp::Lt:
                return CmpOp::Gt;
            case CmpOp::Le:
                return CmpOp::Ge;
            case CmpOp::Gt:
                return CmpOp::Lt;
            case CmpOp::Ge:
                return CmpOp::Le;
        }
        return op;
    }

    Literal parse_literal() {
        if (is(TK::Minus)) {
            next();
            return build_number("-" + parse_number_text());
        }
        if (is(TK::Plus)) {
            next();
            return build_number(parse_number_text());
        }
        if (is(TK::Num)) {
            return build_number(next().text);
        }
        if (is(TK::Str)) {
            return Literal::string(next().text);
        }
        if (is(TK::LBracket)) {
            return Literal::vector(parse_bracket_vector());
        }
        fail_syntax("expected literal, found '" + describe() + "'");
    }

    std::string parse_number_text() {
        if (peek().kind != TK::Num) {
            fail_syntax("expected number, found '" + describe() + "'");
        }
        return next().text;
    }

    Literal build_number(const std::string& text) {
        if (text.find('.') != std::string::npos || text.find('e') != std::string::npos ||
            text.find('E') != std::string::npos) {
            char* end = nullptr;
            errno = 0;
            double value = std::strtod(text.c_str(), &end);
            if (end == text.c_str() || *end != '\0' || errno == ERANGE) {
                fail_syntax("invalid number '" + text + "'");
            }
            return Literal::floating(value);
        }
        try {
            size_t used = 0;
            long long value = std::stoll(text, &used);
            if (used != text.size()) {
                throw std::invalid_argument("trailing characters");
            }
            return Literal::integer(static_cast<int64_t>(value));
        } catch (const std::exception&) {
            throw DbError(1264, "22003", "Out of range value for column: " + text);
        }
    }

    std::vector<double> parse_bracket_vector() {
        expect(TK::LBracket);
        std::vector<double> values;
        if (is(TK::RBracket)) {
            next();
            return values;
        }
        while (true) {
            bool negative = false;
            if (is(TK::Minus)) {
                next();
                negative = true;
            } else if (is(TK::Plus)) {
                next();
            }
            std::string text = parse_number_text();
            if (negative) {
                text = "-" + text;
            }
            char* end = nullptr;
            errno = 0;
            double value = std::strtod(text.c_str(), &end);
            if (end == text.c_str() || *end != '\0' || errno == ERANGE) {
                fail_syntax("invalid vector element '" + text + "'");
            }
            values.push_back(value);
            if (is(TK::Comma)) {
                next();
                continue;
            }
            break;
        }
        expect(TK::RBracket);
        return values;
    }

    std::vector<double> parse_vector_argument() {
        Literal literal = parse_literal();
        if (literal.kind == Literal::Kind::Vector) {
            return literal.vec;
        }
        if (literal.kind == Literal::Kind::Str) {
            std::string trimmed = trim(literal.s);
            if (trimmed.size() < 2 || trimmed.front() != '[' || trimmed.back() != ']') {
                fail_syntax("vector argument must look like '[1.0, 2.0]'");
            }
            std::string_view inner(trimmed.data() + 1, trimmed.size() - 2);
            std::vector<double> values;
            size_t start = 0;
            while (true) {
                size_t comma = inner.find(',', start);
                std::string_view piece = inner.substr(
                    start,
                    comma == std::string::npos ? std::string::npos : comma - start);
                while (!piece.empty() && std::isspace(static_cast<unsigned char>(piece.front()))) piece.remove_prefix(1);
                while (!piece.empty() && std::isspace(static_cast<unsigned char>(piece.back()))) piece.remove_suffix(1);
                if (piece.empty()) {
                    fail_syntax("invalid vector element: empty value");
                }
                const char* begin = piece.data();
                const char* end = begin + piece.size();
                if (*begin == '+') {
                    ++begin;
                    if (begin == end || *begin == '+' || *begin == '-') {
                        fail_syntax("invalid vector element '" + std::string(piece) + "'");
                    }
                }
                double value = 0.0;
                const auto parsed = std::from_chars(begin, end, value, std::chars_format::general);
                if (parsed.ec != std::errc() || parsed.ptr != end) {
                    // Preserve the existing strtod handling for uncommon
                    // numeric syntax; the decimal fast path allocates nothing.
                    std::string fallback(piece);
                    char* tail = nullptr;
                    errno = 0;
                    value = std::strtod(fallback.c_str(), &tail);
                    if (tail == fallback.c_str() || *tail != '\0' || errno == ERANGE)
                        fail_syntax("invalid vector element '" + fallback + "'");
                }
                values.push_back(value);
                if (comma == std::string::npos) {
                    break;
                }
                start = comma + 1;
            }
            return values;
        }
        fail_syntax("vector argument must be a vector literal like '[1.0, 2.0]'");
    }

    uint64_t parse_unsigned_int() {
        if (peek().kind != TK::Num) {
            fail_syntax("expected integer, found '" + describe() + "'");
        }
        std::string text = next().text;
        try {
            size_t used = 0;
            unsigned long long value = std::stoull(text, &used);
            if (used != text.size()) {
                throw std::invalid_argument("trailing characters");
            }
            return static_cast<uint64_t>(value);
        } catch (const std::exception&) {
            fail_syntax("invalid unsigned integer '" + text + "'");
        }
    }

    Stmt parse_set() {
        expect_kw("SET");
        while (is(TK::At) || is_kw("GLOBAL") || is_kw("SESSION")) {
            next();
        }
        if (is_kw("AUTOCOMMIT")) {
            next();
            if (!is(TK::Eq)) {
                while (peek().kind != TK::End) {
                    next();
                }
                Stmt stmt;
                stmt.kind = StmtKind::Noop;
                return stmt;
            }
            next();
            bool enabled = false;
            bool valid = true;
            if (peek().kind == TK::Num && peek().text == "0") {
                enabled = false;
                next();
            } else if (peek().kind == TK::Num && peek().text == "1") {
                enabled = true;
                next();
            } else if (is_kw("OFF")) {
                enabled = false;
                next();
            } else if (is_kw("ON")) {
                enabled = true;
                next();
            } else {
                valid = false;
            }
            while (peek().kind != TK::End) {
                next();
            }
            Stmt stmt;
            stmt.kind = valid ? StmtKind::SetAutocommit : StmtKind::Noop;
            stmt.autocommit = enabled;
            return stmt;
        }
        while (peek().kind != TK::End) {
            next();
        }
        Stmt stmt;
        stmt.kind = StmtKind::Noop;
        return stmt;
    }
};

}  // namespace

Literal Literal::integer(int64_t value) {
    Literal literal;
    literal.kind = Kind::Int;
    literal.i = value;
    return literal;
}

Literal Literal::floating(double value) {
    Literal literal;
    literal.kind = Kind::Float;
    literal.f = value;
    return literal;
}

Literal Literal::string(std::string value) {
    Literal literal;
    literal.kind = Kind::Str;
    literal.s = std::move(value);
    return literal;
}

Literal Literal::vector(std::vector<double> values) {
    Literal literal;
    literal.kind = Kind::Vector;
    literal.vec = std::move(values);
    return literal;
}

Stmt parse_statement(const std::string& input) {
    std::vector<Token> tokens = tokenize(input);
    if (tokens.empty()) {
        fail_syntax("empty statement");
    }
    Parser parser(std::move(tokens));
    return parser.parse();
}

bool for_each_insert_chunk(
    const std::string& sql,
    size_t rows_per_chunk,
    const std::function<bool(const std::string&)>& callback) {
    if (rows_per_chunk == 0) {
        return false;
    }
    size_t begin = 0;
    while (begin < sql.size() &&
           std::isspace(static_cast<unsigned char>(sql[begin]))) {
        ++begin;
    }
    if (sql.size() - begin < 6 ||
        !iequals(std::string_view(sql).substr(begin, 6), "INSERT")) {
        return false;
    }
    size_t cursor = begin;
    char quote = 0;
    size_t values_end = std::string::npos;
    while (cursor < sql.size()) {
        char ch = sql[cursor];
        if (quote != 0) {
            if (ch == '\\' && quote != '`') {
                cursor += std::min<size_t>(2, sql.size() - cursor);
                continue;
            }
            if (ch == quote) {
                if (quote == '\'' && cursor + 1 < sql.size() &&
                    sql[cursor + 1] == '\'') {
                    cursor += 2;
                    continue;
                }
                quote = 0;
            }
            ++cursor;
            continue;
        }
        if (ch == '\'' || ch == '"' || ch == '`') {
            quote = ch;
            ++cursor;
            continue;
        }
        if (cursor + 6 <= sql.size() &&
            iequals(std::string_view(sql).substr(cursor, 6), "VALUES") &&
            (cursor == 0 ||
             !std::isalnum(static_cast<unsigned char>(sql[cursor - 1]))) &&
            (cursor + 6 == sql.size() ||
             !std::isalnum(static_cast<unsigned char>(sql[cursor + 6])))) {
            values_end = cursor + 6;
            break;
        }
        ++cursor;
    }
    if (values_end == std::string::npos) {
        return false;
    }
    std::string prefix = trim(sql.substr(begin, values_end - begin));
    cursor = values_end;
    while (cursor < sql.size() &&
           std::isspace(static_cast<unsigned char>(sql[cursor]))) {
        ++cursor;
    }
    if (cursor >= sql.size() || sql[cursor] != '(' || prefix.empty()) {
        return false;
    }

    size_t batch_start = cursor;
    size_t row_count = 0;
    while (cursor < sql.size()) {
        if (sql[cursor] != '(') {
            return false;
        }
        size_t depth = 0;
        char row_quote = 0;
        while (cursor < sql.size()) {
            char ch = sql[cursor];
            if (row_quote != 0) {
                if (ch == '\\' && row_quote != '`') {
                    cursor += std::min<size_t>(2, sql.size() - cursor);
                    continue;
                }
                if (ch == row_quote) {
                    if (row_quote == '\'' && cursor + 1 < sql.size() &&
                        sql[cursor + 1] == '\'') {
                        cursor += 2;
                        continue;
                    }
                    row_quote = 0;
                }
                ++cursor;
                continue;
            }
            if (ch == '\'' || ch == '"' || ch == '`') {
                row_quote = ch;
                ++cursor;
                continue;
            }
            if (ch == '(') {
                ++depth;
            } else if (ch == ')') {
                if (depth == 0) {
                    return false;
                }
                --depth;
                if (depth == 0) {
                    ++cursor;
                    break;
                }
            }
            ++cursor;
        }
        if (depth != 0 || row_quote != 0) {
            return false;
        }
        ++row_count;
        while (cursor < sql.size() &&
               std::isspace(static_cast<unsigned char>(sql[cursor]))) {
            ++cursor;
        }
        const bool chunk_full = row_count == rows_per_chunk;
        const bool at_end = cursor == sql.size() || sql[cursor] == ';';
        const size_t chunk_end = cursor;
        if (at_end) {
            // Match the ordinary parser: only whitespace and semicolons may
            // follow the last VALUES row. Anything else needs full parsing.
            while (cursor < sql.size() &&
                   (sql[cursor] == ';' ||
                    std::isspace(static_cast<unsigned char>(sql[cursor])))) {
                ++cursor;
            }
            if (cursor != sql.size()) {
                return false;
            }
        } else if (sql[cursor] != ',') {
            return false;
        }
        if (chunk_full || at_end) {
            std::string chunk = prefix + " " +
                                sql.substr(batch_start, chunk_end - batch_start);
            if (!callback(chunk)) {
                return false;
            }
            row_count = 0;
        }
        if (at_end) {
            return true;
        }
        ++cursor;
        while (cursor < sql.size() &&
               std::isspace(static_cast<unsigned char>(sql[cursor]))) {
            ++cursor;
        }
        if (chunk_full) {
            batch_start = cursor;
        }
    }
    return false;
}
