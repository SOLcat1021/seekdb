#include "sql/engine/basic/ob_file_sql_jsonl_reader.h"

#include <rapidjson/document.h>
#include <rapidjson/error/en.h>

#include <iomanip>
#include <sstream>
#include <sys/stat.h>
#include <unordered_set>

namespace oceanbase {
namespace sql {
namespace file_sql {
namespace {

constexpr size_t MAX_LINE_BYTES = 16 * 1024 * 1024;
constexpr size_t MAX_COLUMNS = 4096;

std::string ascii_lower(const std::string &value)
{
  std::string result = value;
  for (char &ch : result) {
    if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
  }
  return result;
}

} // namespace

bool JsonlReader::capture_identity(std::string &error)
{
  struct stat info;
  if (::stat(path_.c_str(), &info) != 0 || !S_ISREG(info.st_mode)) {
    error = "JSONL path is not a readable regular file: " + path_;
    return false;
  }
  file_size_ = static_cast<int64_t>(info.st_size);
  file_mtime_ = static_cast<int64_t>(info.st_mtime);
#ifndef _WIN32
  file_mtime_ = file_mtime_ * 1000000000 + info.st_mtim.tv_nsec;
#endif
  file_device_ = static_cast<uint64_t>(info.st_dev);
  file_inode_ = static_cast<uint64_t>(info.st_ino);
  return true;
}

bool JsonlReader::check_unchanged(std::string &error) const
{
  struct stat info;
  if (::stat(path_.c_str(), &info) != 0 || !S_ISREG(info.st_mode) ||
      static_cast<int64_t>(info.st_size) != file_size_ ||
      static_cast<uint64_t>(info.st_dev) != file_device_ ||
      static_cast<uint64_t>(info.st_ino) != file_inode_
#ifndef _WIN32
      || static_cast<int64_t>(info.st_mtime) * 1000000000 + info.st_mtim.tv_nsec != file_mtime_
#else
      || static_cast<int64_t>(info.st_mtime) != file_mtime_
#endif
      ) {
    error = "JSONL file changed during scan: " + path_;
    return false;
  }
  return true;
}

bool JsonlReader::open_stream(std::string &error)
{
  stream_.close();
  stream_.clear();
  stream_.open(path_.c_str(), std::ios::in | std::ios::binary);
  if (!stream_) {
    error = "Cannot open JSONL file: " + path_;
    return false;
  }
  line_number_ = 0;
  return true;
}

bool JsonlReader::open(const std::string &path, std::string &error)
{
  error.clear();
  path_ = path;
  columns_.clear();
  column_index_.clear();
  schema_ready_ = false;
  return !path.empty() && capture_identity(error) && open_stream(error) && check_unchanged(error);
}

bool JsonlReader::read_line(std::string &line, bool &end, std::string &error)
{
  line.clear();
  end = false;
  for (;;) {
    const int next = stream_.get();
    if (next == EOF) {
      if (stream_.bad()) {
        error = "I/O error reading JSONL line " + std::to_string(line_number_ + 1);
        return false;
      }
      if (line.empty()) {
        end = true;
      } else {
        ++line_number_;
      }
      return true;
    }
    if (next == '\n') {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      ++line_number_;
      return true;
    }
    if (line.size() >= MAX_LINE_BYTES) {
      error = "JSONL line exceeds size limit at line " + std::to_string(line_number_ + 1);
      return false;
    }
    line.push_back(static_cast<char>(next));
  }
}

bool JsonlReader::parse_line(const std::string &line, std::vector<Field> &fields,
                             std::string &error) const
{
  fields.clear();
  if (line.find('\0') != std::string::npos) {
    error = "NUL byte in JSONL line " + std::to_string(line_number_);
    return false;
  }
  rapidjson::Document document;
  document.Parse<rapidjson::kParseValidateEncodingFlag>(line.c_str());
  if (document.HasParseError() || !document.IsObject()) {
    error = "Invalid JSON object at line " + std::to_string(line_number_) + ": " +
            (document.HasParseError() ? rapidjson::GetParseError_En(document.GetParseError()) :
                                        "expected object");
    return false;
  }
  if (document.MemberCount() > MAX_COLUMNS) {
    error = "JSONL column limit exceeded at line " + std::to_string(line_number_);
    return false;
  }
  std::unordered_set<std::string> row_names;
  for (auto member = document.MemberBegin(); member != document.MemberEnd(); ++member) {
    Field field;
    field.name.assign(member->name.GetString(), member->name.GetStringLength());
    if (field.name.empty() || !row_names.insert(ascii_lower(field.name)).second) {
      error = "Duplicate or empty JSONL key at line " + std::to_string(line_number_);
      return false;
    }
    const rapidjson::Value &value = member->value;
    if (value.IsNull()) {
      field.is_null = true;
    } else if (value.IsString()) {
      field.is_null = false;
      field.value.assign(value.GetString(), value.GetStringLength());
      const CsvType parsed = classify_csv_value(field.value);
      field.type = parsed == CsvType::DATE || parsed == CsvType::DATETIME ? parsed : CsvType::VARCHAR;
    } else if (value.IsBool()) {
      field.is_null = false;
      field.value = value.GetBool() ? "true" : "false";
      field.type = CsvType::BOOLEAN;
    } else if (value.IsInt64()) {
      field.is_null = false;
      field.value = std::to_string(value.GetInt64());
      field.type = CsvType::BIGINT;
    } else if (value.IsNumber()) {
      field.is_null = false;
      std::ostringstream formatted;
      formatted << std::setprecision(17) << value.GetDouble();
      field.value = formatted.str();
      field.type = CsvType::DOUBLE;
    } else {
      error = "Nested JSON value is not supported at line " + std::to_string(line_number_);
      return false;
    }
    fields.push_back(std::move(field));
  }
  return true;
}

bool JsonlReader::infer_schema(std::string &error)
{
  error.clear();
  if (!check_unchanged(error) || !open_stream(error)) return false;
  columns_.clear();
  column_index_.clear();
  schema_ready_ = false;
  std::string line;
  std::vector<Field> fields;
  bool end = false;
  while (read_line(line, end, error) && !end) {
    if (!parse_line(line, fields, error)) return false;
    for (const Field &field : fields) {
      const std::string key = ascii_lower(field.name);
      const auto found = column_index_.find(key);
      if (found == column_index_.end()) {
        if (columns_.size() >= MAX_COLUMNS) {
          error = "JSONL schema exceeds column limit";
          return false;
        }
        column_index_[key] = columns_.size();
        columns_.push_back({field.name, field.type});
      } else {
        columns_[found->second].type = widen_csv_type(columns_[found->second].type, field.type);
      }
    }
  }
  if (!error.empty() || columns_.empty() || !check_unchanged(error)) {
    if (columns_.empty() && error.empty()) error = "JSONL file has no columns";
    return false;
  }
  schema_ready_ = true;
  return rewind(error);
}

bool JsonlReader::rewind(std::string &error)
{
  if (!schema_ready_) {
    error = "JSONL schema has not been inferred";
    return false;
  }
  return check_unchanged(error) && open_stream(error);
}

bool JsonlReader::next(std::vector<std::string> &values, std::vector<bool> &is_null,
                       bool &end, std::string &error)
{
  error.clear();
  if (!schema_ready_) {
    error = "JSONL schema has not been inferred";
    return false;
  }
  std::string line;
  std::vector<Field> fields;
  if (!check_unchanged(error) || !read_line(line, end, error)) return false;
  values.assign(columns_.size(), std::string());
  is_null.assign(columns_.size(), true);
  if (end) return true;
  if (!parse_line(line, fields, error)) return false;
  for (const Field &field : fields) {
    const auto found = column_index_.find(ascii_lower(field.name));
    if (found == column_index_.end()) {
      error = "JSONL schema changed during scan";
      return false;
    }
    values[found->second] = field.value;
    is_null[found->second] = field.is_null;
  }
  return true;
}

} // namespace file_sql
} // namespace sql
} // namespace oceanbase
