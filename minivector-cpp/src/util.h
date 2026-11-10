#pragma once

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

enum class TypeKind : uint8_t { BigInt = 1, Varchar = 2, Vector = 3 };

struct ColumnType {
    TypeKind kind;
    uint32_t n = 0;

    static ColumnType bigint() { return ColumnType{TypeKind::BigInt, 0}; }
    static ColumnType varchar(uint32_t n) { return ColumnType{TypeKind::Varchar, n}; }
    static ColumnType vector(uint32_t n) { return ColumnType{TypeKind::Vector, n}; }
    std::string name() const;
};

struct Column {
    std::string name;
    ColumnType type;
};

struct Schema {
    std::string table;
    std::vector<Column> columns;
    size_t pk = 0;
    // Multiple named indexes may share the one physical vector index.
    std::vector<std::string> vector_indexes;

    bool has_vector_index() const { return !vector_indexes.empty(); }
    std::optional<size_t> col_index(std::string_view name) const;
    std::optional<size_t> vector_col() const;
};

struct Value {
    enum class Kind : uint8_t { Int, Str, Vec };
    Kind kind = Kind::Int;
    int64_t i = 0;
    std::string s;
    std::vector<float> v;

    static Value integer(int64_t x);
    static Value string(std::string x);
    static Value vector(std::vector<float> x);
    bool operator==(const Value& other) const;
    bool operator!=(const Value& other) const { return !(*this == other); }
    std::string type_name() const;
};

struct Row {
    std::vector<Value> values;
    bool operator==(const Row& other) const { return values == other.values; }
    bool operator!=(const Row& other) const { return !(*this == other); }
};

struct DbError : std::runtime_error {
    int code;
    std::string state;

    DbError(int code, std::string state, std::string message)
        : std::runtime_error(std::move(message)), code(code), state(std::move(state)) {}

    std::string format() const;
};

[[noreturn]] inline void fail_syntax(const std::string& message) {
    throw DbError(1064, "42000", message);
}
[[noreturn]] inline void fail_internal(const std::string& message) {
    throw DbError(1105, "HY000", message);
}
[[noreturn]] inline void fail_io(const std::string& context, int error) {
    throw DbError(1105, "HY000", context + ": " + std::strerror(error));
}

bool iequals(std::string_view a, std::string_view b);
std::string lower_ascii(std::string_view text);
std::string trim(std::string_view text);
uint32_t utf8_length(const std::string& text);

std::vector<float> parse_vector_literal(
    const std::string& text,
    uint32_t dim,
    const std::string& column,
    size_t row_no);
float f64_to_f32(double value, const std::string& column, size_t row_no);
std::vector<float> vector_from_f64(
    const std::vector<double>& values,
    uint32_t dim,
    const std::string& column,
    size_t row_no);

double l2_distance(const std::vector<float>& a, const std::vector<float>& b);
std::string format_f32(float value);
std::string format_f64(double value);
std::string format_vector(const std::vector<float>& values);
std::string escape_tsv(const std::string& value);
size_t row_mem_size(const Row& row);

// Deterministic crash injection for storage/recovery tests.  The environment
// variable MVDB_CRASH_AT accepts "<point>" or "<point>:<n>" and exits the
// process with code 137 at the selected occurrence.
bool crash_point_hit(const char* point);
void crash_point(const char* point);
