#include "engine.h"
#include "sql.h"
#include "util.h"
#include "text_scan.h"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <optional>
#include <set>
#include <string>
#include <sys/resource.h>
#include <unistd.h>
#include <vector>

namespace {

volatile sig_atomic_t stop_requested = 0;

void request_stop(int) {
    stop_requested = 1;
    ::close(STDIN_FILENO);
}

size_t memory_limit_bytes() {
    const char* paths[] = {
        "/sys/fs/cgroup/memory.max",
        "/sys/fs/cgroup/memory/memory.limit_in_bytes",
    };
    size_t best = 0;
    auto consider = [&](size_t value) {
        if (value == 0 || value >= (1ull << 40)) {
            return;
        }
        best = best == 0 ? value : std::min(best, value);
    };
    for (const char* path : paths) {
        std::ifstream input(path);
        std::string text;
        if (!(input >> text) || text == "max") {
            continue;
        }
        try {
            consider(static_cast<size_t>(std::stoull(text)));
        } catch (...) {
        }
    }
    struct rlimit limit {};
    if (::getrlimit(RLIMIT_AS, &limit) == 0 &&
        limit.rlim_cur != RLIM_INFINITY) {
        consider(static_cast<size_t>(limit.rlim_cur));
    }
    return best;
}

size_t adaptive_hot_budget_mb() {
    constexpr size_t kDefaultMb = 64;
    constexpr size_t kDivisor = 3;
    constexpr size_t kHardCapMb = 768;
    size_t limit = memory_limit_bytes();
    if (limit == 0) {
        return kDefaultMb;
    }
    size_t budget = limit / (1024ull * 1024ull) / kDivisor;
    budget = std::max<size_t>(budget, 1);
    return std::min(budget, kHardCapMb);
}

size_t ann_budget_from_value(size_t ann_max_mb) {
    if (ann_max_mb > 0) {
        return ann_max_mb * 1024ull * 1024ull;
    }
    size_t limit = memory_limit_bytes();
    if (limit == 0) {
        return 0;
    }
    return static_cast<size_t>(limit * 0.8);
}

struct JsonRequest {
    std::string id_json;
    bool has_id = false;
    std::string op;
    std::string sql;
    std::string mode = "exact";
};

class JsonParser {
public:
    explicit JsonParser(std::string_view text) : text_(text) {}

    bool parse(JsonRequest& request) {
        skip_space();
        if (!consume('{')) {
            return false;
        }
        skip_space();
        if (consume('}')) {
            skip_space();
            return position_ == text_.size();
        }
        std::set<std::string> seen;
        while (true) {
            std::string key;
            if (!parse_string(key)) {
                return false;
            }
            if (!seen.insert(key).second) {
                return false;
            }
            skip_space();
            if (!consume(':')) {
                return false;
            }
            skip_space();
            if (key == "id") {
                size_t start = position_;
                if (!skip_value(0)) {
                    return false;
                }
                request.id_json.assign(
                    text_.data() + start, position_ - start);
                request.has_id = true;
            } else if (key == "op") {
                if (!parse_string(request.op)) {
                    return false;
                }
            } else if (key == "sql") {
                if (!parse_string(request.sql)) {
                    return false;
                }
            } else if (key == "search_mode") {
                if (!parse_string(request.mode)) {
                    return false;
                }
            } else if (!skip_value(0)) {
                return false;
            }
            skip_space();
            if (consume('}')) {
                break;
            }
            if (!consume(',')) {
                return false;
            }
            skip_space();
        }
        skip_space();
        return position_ == text_.size();
    }

private:
    static void append_utf8(std::string& out, uint32_t codepoint) {
        if (codepoint <= 0x7F) {
            out.push_back(static_cast<char>(codepoint));
        } else if (codepoint <= 0x7FF) {
            out.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
            out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        } else if (codepoint <= 0xFFFF) {
            out.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
            out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
            out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        }
    }

    bool parse_hex4(uint32_t& value) {
        if (position_ + 4 > text_.size()) {
            return false;
        }
        value = 0;
        for (int index = 0; index < 4; ++index) {
            char ch = text_[position_++];
            value <<= 4;
            if (ch >= '0' && ch <= '9') {
                value |= static_cast<uint32_t>(ch - '0');
            } else if (ch >= 'a' && ch <= 'f') {
                value |= static_cast<uint32_t>(ch - 'a' + 10);
            } else if (ch >= 'A' && ch <= 'F') {
                value |= static_cast<uint32_t>(ch - 'A' + 10);
            } else {
                return false;
            }
        }
        return true;
    }

    bool parse_string(std::string& out) {
        if (!consume('"')) {
            return false;
        }
        out.clear();
        while (position_ < text_.size()) {
            const size_t span = plain_text_span(text_.data() + position_, text_.size() - position_, '"', true);
            out.append(text_.data() + position_, span);
            position_ += span;
            if (position_ == text_.size()) return false;
            unsigned char ch = static_cast<unsigned char>(text_[position_++]);
            if (ch == '"') {
                return true;
            }
            if (ch < 0x20) {
                return false;
            }
            if (ch != '\\') {
                out.push_back(static_cast<char>(ch));
                continue;
            }
            if (position_ >= text_.size()) {
                return false;
            }
            char escaped = text_[position_++];
            switch (escaped) {
                case '"':
                case '\\':
                case '/':
                    out.push_back(escaped);
                    break;
                case 'b':
                    out.push_back('\b');
                    break;
                case 'f':
                    out.push_back('\f');
                    break;
                case 'n':
                    out.push_back('\n');
                    break;
                case 'r':
                    out.push_back('\r');
                    break;
                case 't':
                    out.push_back('\t');
                    break;
                case 'u': {
                    uint32_t codepoint = 0;
                    if (!parse_hex4(codepoint)) {
                        return false;
                    }
                    if (codepoint >= 0xD800 && codepoint <= 0xDBFF) {
                        if (position_ + 2 > text_.size() ||
                            text_[position_] != '\\' ||
                            text_[position_ + 1] != 'u') {
                            return false;
                        }
                        position_ += 2;
                        uint32_t low = 0;
                        if (!parse_hex4(low) || low < 0xDC00 || low > 0xDFFF) {
                            return false;
                        }
                        codepoint =
                            0x10000 + ((codepoint - 0xD800) << 10) + (low - 0xDC00);
                    }
                    append_utf8(out, codepoint);
                    break;
                }
                default:
                    return false;
            }
        }
        return false;
    }

    bool parse_int(int64_t& value) {
        size_t start = position_;
        if (position_ < text_.size() && text_[position_] == '-') {
            ++position_;
        }
        size_t digits = position_;
        while (position_ < text_.size() &&
               std::isdigit(static_cast<unsigned char>(text_[position_]))) {
            ++position_;
        }
        if (digits == position_) {
            return false;
        }
        auto result = std::from_chars(
            text_.data() + start, text_.data() + position_, value);
        return result.ec == std::errc() &&
               result.ptr == text_.data() + position_;
    }

    bool skip_value(int depth) {
        if (depth > 32) {
            return false;
        }
        skip_space();
        if (position_ >= text_.size()) {
            return false;
        }
        char ch = text_[position_];
        if (ch == '"') {
            std::string ignored;
            return parse_string(ignored);
        }
        if (ch == '{') {
            ++position_;
            skip_space();
            if (consume('}')) {
                return true;
            }
            while (true) {
                std::string key;
                if (!parse_string(key)) {
                    return false;
                }
                skip_space();
                if (!consume(':') || !skip_value(depth + 1)) {
                    return false;
                }
                skip_space();
                if (consume('}')) {
                    return true;
                }
                if (!consume(',')) {
                    return false;
                }
                skip_space();
            }
        }
        if (ch == '[') {
            ++position_;
            skip_space();
            if (consume(']')) {
                return true;
            }
            while (true) {
                if (!skip_value(depth + 1)) {
                    return false;
                }
                skip_space();
                if (consume(']')) {
                    return true;
                }
                if (!consume(',')) {
                    return false;
                }
                skip_space();
            }
        }
        if (text_.substr(position_, 4) == "true" ||
            text_.substr(position_, 4) == "null") {
            position_ += 4;
            return true;
        }
        if (text_.substr(position_, 5) == "false") {
            position_ += 5;
            return true;
        }
        size_t start = position_;
        if (text_[position_] == '-') {
            ++position_;
        }
        while (position_ < text_.size()) {
            char current = text_[position_];
            if (std::isdigit(static_cast<unsigned char>(current)) ||
                current == '.' || current == 'e' || current == 'E' ||
                current == '+' || current == '-') {
                ++position_;
            } else {
                break;
            }
        }
        return position_ > start;
    }

    bool consume(char expected) {
        if (position_ < text_.size() && text_[position_] == expected) {
            ++position_;
            return true;
        }
        return false;
    }

    void skip_space() {
        while (position_ < text_.size() &&
               std::isspace(static_cast<unsigned char>(text_[position_]))) {
            ++position_;
        }
    }

    std::string_view text_;
    size_t position_ = 0;
};

std::string json_quote(std::string_view text) {
    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(text.size() + 2);
    out.push_back('"');
    for (unsigned char ch : text) {
        switch (ch) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\b':
                out += "\\b";
                break;
            case '\f':
                out += "\\f";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (ch < 0x20) {
                    out += "\\u00";
                    out.push_back(hex[ch >> 4]);
                    out.push_back(hex[ch & 0x0F]);
                } else {
                    out.push_back(static_cast<char>(ch));
                }
                break;
        }
    }
    out.push_back('"');
    return out;
}

void append_json_value(std::string& out, const OutputValue& value) {
    switch (value.kind) {
        case OutputValue::Kind::Int:
            out += std::to_string(value.i);
            return;
        case OutputValue::Kind::Str:
            out += json_quote(value.s);
            return;
        case OutputValue::Kind::Float:
            out += std::isfinite(value.f) ? format_f64(value.f) : "0";
            return;
        case OutputValue::Kind::Vec:
            out.push_back('[');
            for (size_t index = 0; index < value.v.size(); ++index) {
                if (index > 0) {
                    out.push_back(',');
                }
                float element = value.v[index];
                out += std::isfinite(element) ? format_f32(element) : "0";
            }
            out.push_back(']');
            return;
    }
}

std::string json_error_code(const DbError& error) {
    std::string message = error.what();
    if (message.find("response byte limit") != std::string::npos) {
        return "response_too_large";
    }
    if (error.code == 1366 && message.find("dimensions") != std::string::npos) {
        return "dimension_mismatch";
    }
    if ((error.code == 1105 || error.code == 1235) &&
        (message.find("transaction is already in progress") != std::string::npos ||
         message.find("inside a transaction") != std::string::npos)) {
        return "transaction_state";
    }
    switch (error.code) {
        case 1050:
            return "table_exists";
        case 1054:
            return "column_not_found";
        case 1060:
            return "duplicate_column";
        case 1061:
            return "index_exists";
        case 1062:
            return "duplicate_key";
        case 1064:
            return "syntax_error";
        case 1068:
            return "multiple_primary_key";
        case 1110:
            return "duplicate_column";
        case 1136:
            return "column_count_mismatch";
        case 1146:
            return "table_not_found";
        case 1235:
            return "unsupported_sql";
        case 1264:
            return "out_of_range";
        case 1364:
            return "missing_value";
        case 1366:
            return "invalid_value";
        case 1406:
            return "value_too_long";
        default:
            return "error_" + std::to_string(error.code);
    }
}

struct ResponseTooLarge {};

std::string json_error_response(
    const std::string& id_json,
    const std::string& code,
    const std::string& message) {
    std::string out = "{\"id\":";
    out += id_json;
    out += ",\"ok\":false,\"error\":{\"code\":";
    out += json_quote(code);
    out += ",\"message\":";
    out += json_quote(message);
    out += "}}\n";
    return out;
}

std::string json_error_response(
    const std::string& id_json,
    const DbError& error) {
    return json_error_response(
        id_json, json_error_code(error), error.what());
}

std::string json_response(
    const std::string& id_json,
    const Response& response,
    size_t max_response_bytes) {
    std::string out = "{\"id\":";
    out += id_json;
    if (response.kind == Response::Kind::Query) {
        out += ",\"ok\":true,\"columns\":[";
        for (size_t index = 0; index < response.query.columns.size(); ++index) {
            if (index > 0) {
                out.push_back(',');
            }
            out += json_quote(response.query.columns[index]);
        }
        out += "],\"rows\":[";
        bool first_row = true;
        for (const auto& row : response.query.rows) {
            std::string encoded;
            encoded.push_back('[');
            for (size_t index = 0; index < row.size(); ++index) {
                if (index > 0) {
                    encoded.push_back(',');
                }
                append_json_value(encoded, row[index]);
            }
            encoded.push_back(']');
            if (!first_row) {
                out.push_back(',');
            }
            first_row = false;
            out += encoded;
            if (out.size() + 3 > max_response_bytes) {
                throw ResponseTooLarge{};
            }
        }
        out += "]}\n";
        if (out.size() > max_response_bytes) {
            throw ResponseTooLarge{};
        }
        return out;
    }
    out += ",\"ok\":true";
    if (response.affected_present) {
        out += ",\"affected_rows\":";
        out += std::to_string(response.affected);
    }
    out += "}\n";
    if (out.size() > max_response_bytes) {
        throw ResponseTooLarge{};
    }
    return out;
}

struct Config {
    std::string data_dir = "/data";
    size_t cache_mb = 64;
    bool cache_mb_explicit = false;
    uint64_t compact_min = 8ull * 1024ull * 1024ull;
    bool fsync = true;
    bool affected_rows_matched = false;
    size_t threads = 0;
    size_t ann_rerank = 0;
    size_t ann_max_mb = 0;
    size_t max_response_bytes = 16u * 1024u * 1024u;
    std::string mode = "exact";
    std::string protocol = "json";
    std::string ready = "READY";
    std::string ok = "OK";
    bool quiet = false;
};

std::string require_value(const std::vector<std::string>& args, size_t& index, const char* name) {
    ++index;
    if (index >= args.size()) {
        std::cerr << "minivector: " << name << " requires a value\n";
        std::exit(2);
    }
    return args[index];
}

Config parse_args(int argc, char** argv) {
    Config config;
    if (const char* value = std::getenv("MINIVDB_DATA_DIR")) {
        config.data_dir = value;
    }
    if (const char* value = std::getenv("MINIVDB_CACHE_MB")) {
        try {
            config.cache_mb = std::stoull(value);
            config.cache_mb_explicit = true;
        } catch (...) {
        }
    }
    if (const char* value = std::getenv("MINIVDB_COMPACT_MIN")) {
        try {
            config.compact_min = std::stoull(value);
        } catch (...) {
        }
    }
    if (const char* value = std::getenv("MINIVDB_FSYNC")) {
        config.fsync = !(std::string(value) == "0" || iequals(value, "off"));
    }
    if (const char* value = std::getenv("MINIVDB_READY")) {
        config.ready = value;
    }
    if (const char* value = std::getenv("MINIVDB_OK")) {
        config.ok = value;
    }
    if (const char* value = std::getenv("MINIVDB_AFFECTED_ROWS")) {
        config.affected_rows_matched = iequals(value, "matched");
    }
    if (const char* value = std::getenv("MINIVDB_THREADS")) {
        try {
            config.threads = std::stoull(value);
        } catch (...) {
        }
    }
    if (const char* value = std::getenv("MINIVDB_MODE")) {
        config.mode = value;
    } else if (const char* value = std::getenv("MVDB_MODE")) {
        config.mode = value;
    }
    if (const char* value = std::getenv("MINIVDB_PROTOCOL")) {
        config.protocol = value;
    }
    if (const char* value = std::getenv("MINIVDB_ANN_RERANK")) {
        try {
            config.ann_rerank = std::stoull(value);
        } catch (...) {
        }
    }
    if (const char* value = std::getenv("MINIVDB_ANN_MAX_MB")) {
        try {
            config.ann_max_mb = std::stoull(value);
        } catch (...) {
        }
    }
    if (const char* value = std::getenv("MINIVDB_MAX_RESPONSE_BYTES")) {
        try {
            config.max_response_bytes = std::stoull(value);
        } catch (...) {
        }
    }
    std::vector<std::string> args;
    for (int index = 1; index < argc; ++index) {
        args.emplace_back(argv[index]);
    }
    for (size_t index = 0; index < args.size(); ++index) {
        const std::string& argument = args[index];
        if (argument == "--data" || argument == "--data-dir") {
            config.data_dir = require_value(args, index, argument.c_str());
        } else if (argument == "--mode") {
            std::string value = require_value(args, index, "--mode");
            if (!iequals(value, "exact") && !iequals(value, "ann")) {
                std::cerr << "minivector: --mode expects exact or ann\n";
                std::exit(2);
            }
            config.mode = lower_ascii(value);
        } else if (argument == "--protocol") {
            std::string value = lower_ascii(require_value(args, index, "--protocol"));
            if (value != "json" && value != "tab") {
                std::cerr << "minivector: --protocol expects json or tab\n";
                std::exit(2);
            }
            config.protocol = value;
        } else if (argument == "--ann-rerank") {
            std::string value = require_value(args, index, "--ann-rerank");
            try {
                config.ann_rerank = std::stoull(value);
            } catch (...) {
                std::cerr << "minivector: --ann-rerank expects an integer\n";
                std::exit(2);
            }
        } else if (argument == "--ann-max-mb") {
            std::string value = require_value(args, index, "--ann-max-mb");
            try {
                config.ann_max_mb = std::stoull(value);
            } catch (...) {
                std::cerr << "minivector: --ann-max-mb expects an integer\n";
                std::exit(2);
            }
        } else if (argument == "--max-response-bytes") {
            std::string value = require_value(args, index, "--max-response-bytes");
            try {
                config.max_response_bytes = std::stoull(value);
            } catch (...) {
                std::cerr << "minivector: --max-response-bytes expects an integer\n";
                std::exit(2);
            }
        } else if (argument == "--cache-mb") {
            std::string value = require_value(args, index, "--cache-mb");
            try {
                config.cache_mb = std::stoull(value);
                config.cache_mb_explicit = true;
            } catch (...) {
                std::cerr << "minivector: --cache-mb expects an integer\n";
                std::exit(2);
            }
        } else if (argument == "--cache-bytes") {
            std::string value = require_value(args, index, "--cache-bytes");
            try {
                size_t bytes = std::stoull(value);
                constexpr size_t kMiB = 1024ull * 1024ull;
                config.cache_mb = bytes == 0 ? 0 : (bytes + kMiB - 1) / kMiB;
                config.cache_mb_explicit = true;
            } catch (...) {
                std::cerr << "minivector: --cache-bytes expects an integer\n";
                std::exit(2);
            }
        } else if (argument == "--compact-min") {
            std::string value = require_value(args, index, "--compact-min");
            try {
                config.compact_min = std::stoull(value);
            } catch (...) {
                std::cerr << "minivector: --compact-min expects an integer\n";
                std::exit(2);
            }
        } else if (argument == "--fsync") {
            std::string value = require_value(args, index, "--fsync");
            config.fsync = !(value == "0" || iequals(value, "off"));
        } else if (argument == "--no-fsync") {
            config.fsync = false;
        } else if (argument == "--ready") {
            config.ready = require_value(args, index, "--ready");
        } else if (argument == "--ok") {
            config.ok = require_value(args, index, "--ok");
        } else if (argument == "--affected-rows") {
            std::string value = require_value(args, index, "--affected-rows");
            if (iequals(value, "matched")) {
                config.affected_rows_matched = true;
            } else if (iequals(value, "changed")) {
                config.affected_rows_matched = false;
            } else {
                std::cerr << "minivector: --affected-rows expects matched or changed\n";
                std::exit(2);
            }
        } else if (argument == "--threads") {
            std::string value = require_value(args, index, "--threads");
            try {
                config.threads = std::stoull(value);
            } catch (...) {
                std::cerr << "minivector: --threads expects an integer\n";
                std::exit(2);
            }
        } else if (argument == "--quiet" || argument == "-q") {
            config.quiet = true;
        } else if (argument == "--help" || argument == "-h") {
            std::cout
                << "minivector-cpp\n"
                << "Usage: minivector [OPTIONS]\n\n"
                << "  --data <DIR>       data directory (default /data, env MINIVDB_DATA_DIR)\n"
                << "  --data-dir <DIR>   alias for --data\n"
                << "  --protocol <json|tab>  JSONL judge protocol (default) or local tab protocol\n"
                << "  --mode <exact|ann> default query mode (JSON requests may override)\n"
                << "  --ann-rerank <N>   override PQ exact-rerank candidates (0=default)\n"
                << "  --ann-max-mb <N>   disable ANN above this estimated memory budget\n"
                << "  --max-response-bytes <N>  JSONL response wire limit (default 16MiB)\n"
                << "  --cache-mb <N>     HotSet budget in MiB (default adaptive from cgroup/RLIMIT_AS, max 768)\n"
                << "  --cache-bytes <N>  same budget expressed in bytes (rounded up to MiB)\n"
                << "  --compact-min <N>  segment bytes before compaction (default 8MiB)\n"
                << "  --fsync <on|off>   fsync committed records (default on)\n"
                << "  --no-fsync         same as --fsync off\n"
                << "  --ready <TOKEN>    startup ready token (default READY)\n"
                << "  --ok <TOKEN>       per-statement success token (default OK)\n"
                << "  --affected-rows <matched|changed>  UPDATE affected_rows semantics\n"
                << "  --threads <N>      parallel scan threads (0=auto, default 0)\n"
                << "  --quiet            suppress startup logging\n"
                << "  --help             print this help\n";
            std::exit(0);
        } else if (argument == "--version") {
            std::cout << "minivector-cpp 0.1.0\n";
            std::exit(0);
        } else {
            std::cerr << "minivector: unknown argument '" << argument << "'\n";
            std::exit(2);
        }
    }
    if (!config.cache_mb_explicit) {
        config.cache_mb = adaptive_hot_budget_mb();
    }
    return config;
}

enum class SplitState {
    Normal,
    SingleQuote,
    DoubleQuote,
    Backtick,
    LineComment,
    BlockComment,
    BlockCommentStar,
};

class StatementSplitter {
public:
    bool is_empty() const { return trim(buffer_).empty(); }

    std::string peek() const { return trim(buffer_); }

    void clear() {
        buffer_.clear();
        state_ = SplitState::Normal;
        escape_ = false;
        last_ = '\0';
    }

    std::vector<std::string> push_line(const std::string& line) {
        std::vector<std::string> statements;
        for (char ch : line) {
            switch (state_) {
                case SplitState::Normal:
                    switch (ch) {
                        case '\'':
                            state_ = SplitState::SingleQuote;
                            break;
                        case '"':
                            state_ = SplitState::DoubleQuote;
                            break;
                        case '`':
                            state_ = SplitState::Backtick;
                            break;
                        case '#':
                            state_ = SplitState::LineComment;
                            break;
                        case '-':
                            if (last_ == '-') {
                                state_ = SplitState::LineComment;
                            }
                            break;
                        case '*':
                            if (last_ == '/') {
                                state_ = SplitState::BlockComment;
                            }
                            break;
                        case ';': {
                            std::string statement = trim(buffer_);
                            buffer_.clear();
                            if (!statement.empty()) {
                                statements.push_back(std::move(statement));
                            }
                            last_ = ch;
                            continue;
                        }
                        default:
                            break;
                    }
                    buffer_.push_back(ch);
                    break;
                case SplitState::SingleQuote:
                    if (escape_) {
                        escape_ = false;
                    } else if (ch == '\\') {
                        escape_ = true;
                    } else if (ch == '\'') {
                        state_ = SplitState::Normal;
                    }
                    buffer_.push_back(ch);
                    break;
                case SplitState::DoubleQuote:
                    if (escape_) {
                        escape_ = false;
                    } else if (ch == '\\') {
                        escape_ = true;
                    } else if (ch == '"') {
                        state_ = SplitState::Normal;
                    }
                    buffer_.push_back(ch);
                    break;
                case SplitState::Backtick:
                    if (ch == '`') {
                        state_ = SplitState::Normal;
                    }
                    buffer_.push_back(ch);
                    break;
                case SplitState::LineComment:
                    buffer_.push_back(ch);
                    if (ch == '\n') {
                        state_ = SplitState::Normal;
                    }
                    break;
                case SplitState::BlockComment:
                    buffer_.push_back(ch);
                    if (ch == '*') {
                        state_ = SplitState::BlockCommentStar;
                    }
                    break;
                case SplitState::BlockCommentStar:
                    buffer_.push_back(ch);
                    if (ch == '/') {
                        state_ = SplitState::Normal;
                    } else if (ch != '*') {
                        state_ = SplitState::BlockComment;
                    }
                    break;
            }
            last_ = ch;
        }
        if (state_ == SplitState::LineComment) {
            state_ = SplitState::Normal;
        }
        buffer_.push_back('\n');
        return statements;
    }

private:
    std::string buffer_;
    SplitState state_ = SplitState::Normal;
    bool escape_ = false;
    char last_ = '\0';
};

bool is_incomplete_error(const DbError& error) {
    std::string message = error.what();
    return message.find("end of statement") != std::string::npos ||
           message.find("unterminated") != std::string::npos ||
           message.find("found None") != std::string::npos;
}

void print_error(const DbError& error) {
    std::cout << error.format() << '\n';
    std::cout.flush();
}

void print_response(const Response& response, const Config& config) {    if (response.kind == Response::Kind::Query) {
        for (size_t index = 0; index < response.query.columns.size(); ++index) {
            if (index > 0) {
                std::cout << '\t';
            }
            std::cout << response.query.columns[index];
        }
        std::cout << '\n';
        if (!response.query.spill_path.empty()) {
            std::ifstream input(
                response.query.spill_path, std::ios::binary);
            std::string line;
            while (std::getline(input, line)) {
                std::cout << line << '\n';
            }
            if (!input.eof()) {
                std::cerr << "minivector: cannot read result spool "
                          << response.query.spill_path << '\n';
            }
            std::remove(response.query.spill_path.c_str());
            if (!config.ok.empty()) {
                std::cout << config.ok << ' ' << response.query.spill_rows << '\n';
            }
        } else {
            std::string line;
            for (const auto& row : response.query.rows) {
                line.clear();
                for (size_t index = 0; index < row.size(); ++index) {
                    if (index > 0) {
                        line.push_back('\t');
                    }
                    row[index].append_text(line);
                }
                std::cout << line << '\n';
            }
            if (!config.ok.empty()) {
                std::cout << config.ok << ' ' << response.query.rows.size() << '\n';
            }
        }
    } else {
        if (!config.ok.empty()) {
            std::cout << config.ok << ' ' << response.affected << '\n';
        }
    }
    std::cout.flush();
}

void run_parsed(
    Database& database,
    std::optional<Txn>& txn,
    const Stmt& statement,
    const Config& config) {
    try {
        Response response = database.execute(statement, txn);
        print_response(response, config);
    } catch (const DbError& error) {
        print_error(error);
    } catch (const std::exception& error) {
        DbError wrapped(1105, "HY000", error.what());
        print_error(wrapped);
    }
}

void run_text(
    Database& database,
    std::optional<Txn>& txn,
    const std::string& text,
    const Config& config) {
    std::string trimmed = trim(text);
    if (trimmed.empty()) {
        return;
    }
    try {
        if (trimmed.size() > (1u << 20) &&
            iequals(trimmed.substr(0, 6), "INSERT")) {
            Response response = database.execute_large_insert(trimmed, txn);
            print_response(response, config);
            return;
        }
        Stmt statement = parse_statement(trimmed);
        run_parsed(database, txn, statement, config);
    } catch (const DbError& error) {
        print_error(error);
    } catch (const std::exception& error) {
        DbError wrapped(1105, "HY000", error.what());
        print_error(wrapped);
    }
}

void run_json_loop(
    Database& database,
    std::optional<Txn>& txn,
    size_t max_response_bytes) {
    std::string line;
    while (std::getline(std::cin, line)) {
        if (stop_requested) {
            break;
        }
        JsonRequest request;
        JsonParser parser(line);
        if (!parser.parse(request) || !request.has_id || request.op.empty()) {
            DbError error(1064, "42000", "invalid JSON request");
            std::cout << json_error_response("0", error);
            std::cout.flush();
            continue;
        }
        try {
            if (request.op == "shutdown") {
                txn.reset();
                std::cout << json_response(
                    request.id_json, Response::ok(), max_response_bytes);
                std::cout.flush();
                return;
            }
            if (request.op != "execute") {
                throw DbError(1235, "42000", "unsupported operation");
            }
            database.ann_mode = iequals(request.mode, "ann");
            std::string sql = trim(request.sql);
            if (sql.empty()) {
                throw DbError(1064, "42000", "empty SQL");
            }
            Response response;
            if (sql.size() > (1u << 20) &&
                iequals(sql.substr(0, 6), "INSERT")) {
                response = database.execute_large_insert(sql, txn);
            } else {
                Stmt statement = parse_statement(sql);
                response = database.execute(statement, txn);
            }
            std::cout << json_response(
                request.id_json, response, max_response_bytes);
        } catch (const ResponseTooLarge&) {
            std::cout << json_error_response(
                request.id_json,
                "response_too_large",
                "result exceeds the response wire limit");
        } catch (const DbError& error) {
            std::cout << json_error_response(request.id_json, error);
        } catch (const std::exception& error) {
            DbError wrapped(1105, "HY000", error.what());
            std::cout << json_error_response(request.id_json, wrapped);
        }
        std::cout.flush();
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::ios::sync_with_stdio(false);
    std::cin.tie(nullptr);
    Config config = parse_args(argc, argv);
    struct sigaction stop_action {};
    stop_action.sa_handler = request_stop;
    sigemptyset(&stop_action.sa_mask);
    stop_action.sa_flags = 0;  // interrupt getline instead of SA_RESTART
    ::sigaction(SIGTERM, &stop_action, nullptr);
    ::sigaction(SIGINT, &stop_action, nullptr);
    size_t cache_bytes = config.cache_mb * 1024ull * 1024ull;
    auto started = std::chrono::steady_clock::now();
    Database database = Database::open(
        config.data_dir, cache_bytes, config.fsync, config.compact_min);
    database.affected_rows_matched = config.affected_rows_matched;
    database.threads = config.threads;
    database.ann_rerank = config.ann_rerank;
    database.ann_mode = iequals(config.mode, "ann");
    database.spool_results = config.protocol == "tab";
    database.result_limit_bytes =
        config.protocol == "json" ? config.max_response_bytes : 0;
    database.set_ann_budget(ann_budget_from_value(config.ann_max_mb));
    database.preload_ann_indexes();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - started)
                       .count();
    if (!config.quiet) {
        std::cerr << "minivector: data=" << config.data_dir
                  << " tables=" << database.table_count()
                  << " rows=" << database.row_count()
                  << " recovery=" << elapsed << "ms hot=" << config.cache_mb
                  << "MiB fsync=" << (config.fsync ? "on" : "off") << '\n';
    }
    std::optional<Txn> txn;
    if (config.protocol == "json") {
        std::cout << "{\"type\":\"ready\"}\n";
        std::cout.flush();
        run_json_loop(database, txn, config.max_response_bytes);
        return 0;
    }
    if (!config.ready.empty()) {
        std::cout << config.ready << '\n';
        std::cout.flush();
    }

    StatementSplitter splitter;
    std::string line;
    while (std::getline(std::cin, line)) {
        if (stop_requested) {
            break;
        }
        if (splitter.is_empty()) {
            std::string trimmed = trim(line);
            if (iequals(trimmed, "quit") || iequals(trimmed, "exit") || trimmed == "\\q") {
                break;
            }
        }
        std::vector<std::string> statements = splitter.push_line(line);
        for (const std::string& statement : statements) {
            run_text(database, txn, statement, config);
        }
        if (!statements.empty() || splitter.is_empty()) {
            continue;
        }
        std::string candidate = splitter.peek();
        try {
            Stmt statement = parse_statement(candidate);
            splitter.clear();
            run_parsed(database, txn, statement, config);
        } catch (const DbError& error) {
            std::string message = error.what();
            if (message == "empty statement") {
                splitter.clear();
            } else if (!is_incomplete_error(error)) {
                splitter.clear();
                print_error(error);
            }
        } catch (const std::exception& error) {
            splitter.clear();
            DbError wrapped(1105, "HY000", error.what());
            print_error(wrapped);
        }
    }
    if (!stop_requested && !splitter.is_empty()) {
        run_text(database, txn, splitter.peek(), config);
    }
    return 0;
}
