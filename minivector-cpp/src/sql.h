#pragma once

#include "util.h"

#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

enum class CmpOp { Eq, Ne, Lt, Le, Gt, Ge };

struct Literal {
    enum class Kind { Int, Float, Str, Vector };
    Kind kind = Kind::Int;
    int64_t i = 0;
    double f = 0.0;
    std::string s;
    std::vector<double> vec;

    static Literal integer(int64_t value);
    static Literal floating(double value);
    static Literal string(std::string value);
    static Literal vector(std::vector<double> values);
};

struct ColumnDef {
    std::string name;
    ColumnType type;
    bool primary = false;
};

struct Clause {
    std::string column;
    CmpOp op;
    Literal value;
};

struct Cond {
    std::vector<Clause> clauses;
};

enum class SelectItemKind { Star, Column, Distance };

struct SelectItem {
    SelectItemKind kind = SelectItemKind::Column;
    std::string name;
    std::string column;
    std::vector<double> query;
    std::optional<std::string> alias;
};

enum class OrderKind { Name, Distance };

struct OrderBy {
    OrderKind kind = OrderKind::Name;
    std::string name;
    std::string column;
    std::vector<double> query;
    bool asc = true;
};

enum class StmtKind {
    CreateTable,
    CreateIndex,
    Insert,
    Select,
    Update,
    Delete,
    Begin,
    Commit,
    Rollback,
    SetAutocommit,
    Noop,
};

struct Stmt {
    StmtKind kind = StmtKind::Noop;
    std::string table;
    std::string name;
    std::string column;
    bool if_not_exists = false;
    std::vector<ColumnDef> columns;
    std::optional<std::string> table_pk;
    std::optional<std::vector<std::string>> insert_columns;
    std::vector<std::vector<Literal>> rows;
    std::vector<SelectItem> items;
    std::optional<Cond> filter;
    std::vector<OrderBy> orders;
    std::optional<uint64_t> limit;
    std::vector<std::pair<std::string, Literal>> sets;
    bool autocommit = false;
};

Stmt parse_statement(const std::string& input);

// Split a large INSERT ... VALUES statement into syntactically complete
// statements of at most rows_per_chunk rows. Returns false when the text is
// not a shape this lightweight splitter can handle.
bool for_each_insert_chunk(
    const std::string& sql,
    size_t rows_per_chunk,
    const std::function<bool(const std::string&)>& callback);
