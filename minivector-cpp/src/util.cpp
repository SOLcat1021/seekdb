#include "util.h"
#include "exact_scan.h"

#include <cstdlib>
#include <map>
#include <sstream>

std::string ColumnType::name() const {
    switch (kind) {
        case TypeKind::BigInt:
            return "BIGINT";
        case TypeKind::Varchar:
            return "VARCHAR(" + std::to_string(n) + ")";
        case TypeKind::Vector:
            return "VECTOR(" + std::to_string(n) + ")";
    }
    return "UNKNOWN";
}

std::optional<size_t> Schema::col_index(std::string_view name) const {
    for (size_t index = 0; index < columns.size(); ++index) {
        if (iequals(columns[index].name, name)) {
            return index;
        }
    }
    return std::nullopt;
}

std::optional<size_t> Schema::vector_col() const {
    for (size_t index = 0; index < columns.size(); ++index) {
        if (columns[index].type.kind == TypeKind::Vector) {
            return index;
        }
    }
    return std::nullopt;
}

Value Value::integer(int64_t x) {
    Value value;
    value.kind = Kind::Int;
    value.i = x;
    return value;
}

Value Value::string(std::string x) {
    Value value;
    value.kind = Kind::Str;
    value.s = std::move(x);
    return value;
}

Value Value::vector(std::vector<float> x) {
    Value value;
    value.kind = Kind::Vec;
    value.v = std::move(x);
    return value;
}

bool Value::operator==(const Value& other) const {
    if (kind != other.kind) {
        return false;
    }
    switch (kind) {
        case Kind::Int:
            return i == other.i;
        case Kind::Str:
            return s == other.s;
        case Kind::Vec:
            return v == other.v;
    }
    return false;
}

std::string Value::type_name() const {
    switch (kind) {
        case Kind::Int:
            return "BIGINT";
        case Kind::Str:
            return "VARCHAR";
        case Kind::Vec:
            return "VECTOR";
    }
    return "UNKNOWN";
}

std::string DbError::format() const {
    return "ERROR " + std::to_string(code) + " (" + state + "): " + what();
}

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t index = 0; index < a.size(); ++index) {
        unsigned char left = static_cast<unsigned char>(a[index]);
        unsigned char right = static_cast<unsigned char>(b[index]);
        if (std::tolower(left) != std::tolower(right)) {
            return false;
        }
    }
    return true;
}

std::string lower_ascii(std::string_view text) {
    std::string out(text);
    for (char& ch : out) {
        unsigned char byte = static_cast<unsigned char>(ch);
        ch = static_cast<char>(std::tolower(byte));
    }
    return out;
}

std::string trim(std::string_view text) {
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) {
        ++begin;
    }
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
        --end;
    }
    return std::string(text.substr(begin, end - begin));
}

uint32_t utf8_length(const std::string& text) {
    uint32_t count = 0;
    for (unsigned char byte : text) {
        if ((byte & 0xC0) != 0x80) {
            ++count;
        }
    }
    return count;
}

float f64_to_f32(double value, const std::string& column, size_t row_no) {
    if (!std::isfinite(value) || std::fabs(value) > static_cast<double>(std::numeric_limits<float>::max())) {
        throw DbError(
            1366,
            "22007",
            "Incorrect vector value for column '" + column + "' at row " +
                std::to_string(row_no) + ": value is not a finite float32");
    }
    return static_cast<float>(value);
}

std::vector<float> parse_vector_literal(
    const std::string& text,
    uint32_t dim,
    const std::string& column,
    size_t row_no) {
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) {
        ++begin;
    }
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
        --end;
    }
    if (end - begin < 2 || text[begin] != '[' || text[end - 1] != ']') {
        throw DbError(
            1366,
            "22007",
            "Incorrect vector value for column '" + column + "' at row " +
                std::to_string(row_no) + ": missing brackets");
    }
    const char* cursor = text.data() + begin + 1;
    const char* limit = text.data() + end - 1;
    std::vector<float> out;
    out.reserve(dim);
    if (cursor != limit) {
        while (true) {
            while (cursor < limit &&
                   std::isspace(static_cast<unsigned char>(*cursor))) {
                ++cursor;
            }
            const char* token_begin = cursor;
            if (cursor == limit) {
                throw DbError(
                    1366,
                    "22007",
                    "Incorrect vector value for column '" + column + "' at row " +
                        std::to_string(row_no) + ": empty element");
            }
            // std::from_chars does not accept a leading plus; consume one,
            // but reject doubled signs instead of silently reading '+-1' as
            // '-1'.
            if (*cursor == '+') {
                ++cursor;
                if (cursor == limit || *cursor == '+' || *cursor == '-') {
                    std::string piece(
                        token_begin, static_cast<size_t>(limit - token_begin));
                    throw DbError(
                        1366,
                        "22007",
                        "Incorrect vector value for column '" + column +
                            "' at row " + std::to_string(row_no) + ": '" +
                            trim(piece) + "' is not a number");
                }
            }
            double value = 0.0;
            auto parsed = std::from_chars(
                cursor, limit, value, std::chars_format::general);
            if (parsed.ec != std::errc() || parsed.ptr == cursor) {
                std::string piece(
                    token_begin, static_cast<size_t>(limit - token_begin));
                throw DbError(
                    1366,
                    "22007",
                    "Incorrect vector value for column '" + column + "' at row " +
                        std::to_string(row_no) + ": '" + trim(piece) +
                        "' is not a number");
            }
            cursor = parsed.ptr;
            while (cursor < limit &&
                   std::isspace(static_cast<unsigned char>(*cursor))) {
                ++cursor;
            }
            out.push_back(f64_to_f32(value, column, row_no));
            if (cursor == limit) {
                break;
            }
            if (*cursor != ',') {
                std::string piece(
                    token_begin, static_cast<size_t>(limit - token_begin));
                throw DbError(
                    1366,
                    "22007",
                    "Incorrect vector value for column '" + column + "' at row " +
                        std::to_string(row_no) + ": '" + trim(piece) +
                        "' is not a number");
            }
            ++cursor;
        }
    }
    if (out.size() != dim) {
        throw DbError(
            1366,
            "22007",
            "Incorrect vector value for column '" + column + "' at row " +
                std::to_string(row_no) + ": expected " + std::to_string(dim) +
                " dimensions, got " + std::to_string(out.size()));
    }
    return out;
}

std::vector<float> vector_from_f64(
    const std::vector<double>& values,
    uint32_t dim,
    const std::string& column,
    size_t row_no) {
    if (values.size() != dim) {
        throw DbError(
            1366,
            "22007",
            "Incorrect vector value for column '" + column + "' at row " +
                std::to_string(row_no) + ": expected " + std::to_string(dim) +
                " dimensions, got " + std::to_string(values.size()));
    }
    std::vector<float> out;
    out.reserve(values.size());
    for (double value : values) {
        out.push_back(f64_to_f32(value, column, row_no));
    }
    return out;
}

double l2_distance(const std::vector<float>& a, const std::vector<float>& b) {
    return exact_scan::distance(a.data(), b.data(), a.size());
}

static std::string to_chars_f64(double value) {
    if (value == 0.0) {
        return "0";
    }
    std::array<char, 64> buffer{};
    auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (result.ec != std::errc()) {
        return "0";
    }
    return std::string(buffer.data(), result.ptr);
}

static std::string to_chars_f32(float value) {
    if (value == 0.0f) {
        return "0";
    }
    std::array<char, 64> buffer{};
    auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (result.ec != std::errc()) {
        return "0";
    }
    return std::string(buffer.data(), result.ptr);
}

std::string format_f32(float value) {
    return to_chars_f32(value);
}

std::string format_f64(double value) {
    return to_chars_f64(value);
}

std::string format_vector(const std::vector<float>& values) {
    std::string out = "[";
    for (size_t index = 0; index < values.size(); ++index) {
        if (index > 0) {
            out.push_back(',');
        }
        out += format_f32(values[index]);
    }
    out.push_back(']');
    return out;
}

std::string escape_tsv(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (char ch : value) {
        switch (ch) {
            case '\\':
                out += "\\\\";
                break;
            case '\t':
                out += "\\t";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            default:
                out.push_back(ch);
                break;
        }
    }
    return out;
}

size_t row_mem_size(const Row& row) {
    size_t size = 32;
    for (const Value& value : row.values) {
        size += 24;
        switch (value.kind) {
            case Value::Kind::Int:
                size += 8;
                break;
            case Value::Kind::Str:
                size += value.s.size();
                break;
            case Value::Kind::Vec:
                size += value.v.size() * sizeof(float);
                break;
        }
    }
    return size;
}

bool crash_point_hit(const char* point) {
    const char* specification = std::getenv("MVDB_CRASH_AT");
    if (specification == nullptr || *specification == '\0') {
        return false;
    }
    std::string text(specification);
    std::string name = text;
    int target = 1;
    size_t colon = text.find(':');
    if (colon != std::string::npos) {
        name = text.substr(0, colon);
        try {
            target = std::stoi(text.substr(colon + 1));
        } catch (...) {
            target = 1;
        }
    }
    if (target < 1) {
        target = 1;
    }
    if (name != point) {
        return false;
    }
    static std::map<std::string, int> counters;
    ++counters[name];
    return counters[name] == target;
}

void crash_point(const char* point) {
    if (crash_point_hit(point)) {
        std::_Exit(137);
    }
}
