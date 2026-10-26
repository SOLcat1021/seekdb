#include "sql/engine/basic/ob_file_sql_csv_reader.h"

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <sys/stat.h>
#include <unordered_set>

namespace oceanbase {
namespace sql {
namespace file_sql {
namespace {

std::filesystem::path file_path_from_utf8(const std::string &bytes)
{
  return std::filesystem::path(std::u8string(bytes.begin(), bytes.end()));
}

#ifdef _WIN32
using FileStat = struct _stat64;
bool file_stat(const std::string &path, FileStat &info)
{
  return _wstat64(file_path_from_utf8(path).c_str(), &info) == 0 &&
         (info.st_mode & _S_IFMT) == _S_IFREG;
}
#else
using FileStat = struct stat;
bool file_stat(const std::string &path, FileStat &info)
{
  return ::stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode);
}
#endif

std::string ascii_lower(const std::string &value)
{
  std::string result = value;
  for (char &ch : result) {
    if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
  }
  return result;
}

bool digits(const std::string &value, size_t begin, size_t length)
{
  if (begin + length > value.size()) return false;
  for (size_t i = begin; i < begin + length; ++i) {
    if (value[i] < '0' || value[i] > '9') return false;
  }
  return true;
}

int number(const std::string &value, size_t begin, size_t length)
{
  int result = 0;
  for (size_t i = begin; i < begin + length; ++i) result = result * 10 + value[i] - '0';
  return result;
}

bool valid_date(const std::string &value)
{
  if (value.size() < 10 || value[4] != '-' || value[7] != '-' ||
      !digits(value, 0, 4) || !digits(value, 5, 2) || !digits(value, 8, 2)) return false;
  const int year = number(value, 0, 4);
  const int month = number(value, 5, 2);
  const int day = number(value, 8, 2);
  if (year < 1 || month < 1 || month > 12 || day < 1) return false;
  const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  const bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
  return day <= days[month - 1] + (month == 2 && leap ? 1 : 0);
}

bool valid_datetime(const std::string &value)
{
  if (value.size() != 19 || !valid_date(value) ||
      (value[10] != ' ' && value[10] != 'T') ||
      value[13] != ':' || value[16] != ':' ||
      !digits(value, 11, 2) || !digits(value, 14, 2) || !digits(value, 17, 2)) return false;
  return number(value, 11, 2) < 24 && number(value, 14, 2) < 60 &&
         number(value, 17, 2) < 60;
}

bool valid_utf8(const std::string &value)
{
  const unsigned char *bytes = reinterpret_cast<const unsigned char *>(value.data());
  for (size_t i = 0; i < value.size();) {
    const unsigned char first = bytes[i++];
    if (first < 0x80) continue;
    size_t continuation = 0;
    if (first >= 0xC2 && first <= 0xDF) continuation = 1;
    else if (first >= 0xE0 && first <= 0xEF) continuation = 2;
    else if (first >= 0xF0 && first <= 0xF4) continuation = 3;
    else return false;
    if (i + continuation > value.size()) return false;
    const unsigned char second = bytes[i];
    if ((first == 0xE0 && second < 0xA0) || (first == 0xED && second >= 0xA0) ||
        (first == 0xF0 && second < 0x90) || (first == 0xF4 && second >= 0x90)) return false;
    for (size_t j = 0; j < continuation; ++j) {
      if (bytes[i++] < 0x80 || bytes[i - 1] > 0xBF) return false;
    }
  }
  return true;
}

void strip_bom(std::vector<std::string> &values)
{
  if (!values.empty() && values[0].size() >= 3 &&
      static_cast<unsigned char>(values[0][0]) == 0xEF &&
      static_cast<unsigned char>(values[0][1]) == 0xBB &&
      static_cast<unsigned char>(values[0][2]) == 0xBF) {
    values[0].erase(0, 3);
  }
}

} // namespace

CsvType classify_csv_value(const std::string &value)
{
  if (value.empty()) return CsvType::NULL_TYPE;
  if (ascii_lower(value) == "true" || ascii_lower(value) == "false") {
    return CsvType::BOOLEAN;
  }
  if (value.size() == 10 && valid_date(value)) return CsvType::DATE;
  if (valid_datetime(value)) return CsvType::DATETIME;
  char *end = nullptr;
  errno = 0;
  std::strtoll(value.c_str(), &end, 10);
  if (end == value.c_str() + value.size() && errno != ERANGE) return CsvType::BIGINT;
  errno = 0;
  const double floating = std::strtod(value.c_str(), &end);
  if (end == value.c_str() + value.size() && errno != ERANGE && std::isfinite(floating)) {
    return CsvType::DOUBLE;
  }
  return CsvType::VARCHAR;
}

CsvType widen_csv_type(CsvType previous, CsvType next)
{
  if (previous == CsvType::NULL_TYPE) return next;
  if (next == CsvType::NULL_TYPE || previous == next) return previous;
  if ((previous == CsvType::BIGINT && next == CsvType::DOUBLE) ||
      (previous == CsvType::DOUBLE && next == CsvType::BIGINT)) return CsvType::DOUBLE;
  if ((previous == CsvType::DATE && next == CsvType::DATETIME) ||
      (previous == CsvType::DATETIME && next == CsvType::DATE)) return CsvType::DATETIME;
  return CsvType::VARCHAR;
}

bool CsvReader::capture_identity(std::string &error)
{
  FileStat info;
  if (!file_stat(path_, info)) {
    error = "CSV path is not a readable regular file: " + path_;
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

bool CsvReader::check_unchanged(std::string &error) const
{
  FileStat info;
  if (!file_stat(path_, info) ||
      static_cast<int64_t>(info.st_size) != file_size_ ||
      static_cast<uint64_t>(info.st_dev) != file_device_ ||
      static_cast<uint64_t>(info.st_ino) != file_inode_
#ifndef _WIN32
      || static_cast<int64_t>(info.st_mtime) * 1000000000 + info.st_mtim.tv_nsec != file_mtime_
#else
      || static_cast<int64_t>(info.st_mtime) != file_mtime_
#endif
      ) {
    error = "CSV file changed during scan: " + path_;
    return false;
  }
  return true;
}

bool CsvReader::open_stream(std::string &error)
{
  stream_.close();
  stream_.clear();
  stream_.open(file_path_from_utf8(path_), std::ios::in | std::ios::binary);
  if (!stream_) {
    error = "Cannot open CSV file: " + path_;
    return false;
  }
  record_number_ = 0;
  return true;
}

bool CsvReader::open(const std::string &path, const CsvOptions &options, std::string &error)
{
  error.clear();
  if (path.empty() || options.delimiter == options.quote || options.max_record_bytes == 0 ||
      options.max_columns == 0) {
    error = "Invalid CSV path or options";
    return false;
  }
  path_ = path;
  options_ = options;
  columns_.clear();
  schema_ready_ = false;
  return capture_identity(error) && open_stream(error) && check_unchanged(error);
}

bool CsvReader::read_record(std::vector<std::string> &values, bool &end,
                            std::string &error)
{
  values.clear();
  quoted_fields_.clear();
  end = false;
  std::string field;
  bool quoted = false;
  bool after_quote = false;
  bool at_field_start = true;
  bool field_quoted = false;
  bool saw_char = false;
  size_t bytes = 0;
  for (;;) {
    const int next = stream_.get();
    if (next == EOF) {
      if (stream_.bad()) {
        error = "I/O error reading CSV record " + std::to_string(record_number_ + 1);
        return false;
      }
      if (quoted) {
        error = "Unterminated CSV quote at record " + std::to_string(record_number_ + 1);
        return false;
      }
      if (!saw_char && field.empty() && values.empty()) {
        end = true;
        return true;
      }
      if (!valid_utf8(field)) {
        error = "Invalid UTF-8 in CSV record " + std::to_string(record_number_ + 1);
        return false;
      }
      values.push_back(field);
      quoted_fields_.push_back(field_quoted);
      if (values.size() > options_.max_columns) {
        error = "CSV column limit exceeded at record " + std::to_string(record_number_ + 1);
        return false;
      }
      if (record_number_ == 0) strip_bom(values);
      ++record_number_;
      return true;
    }
    const char ch = static_cast<char>(next);
    saw_char = true;
    if (++bytes > options_.max_record_bytes) {
      error = "CSV record exceeds size limit at record " + std::to_string(record_number_ + 1);
      return false;
    }
    if (quoted) {
      if (ch == options_.quote) {
        if (stream_.peek() == options_.quote) {
          stream_.get();
          ++bytes;
          if (bytes > options_.max_record_bytes) {
            error = "CSV record exceeds size limit at record " + std::to_string(record_number_ + 1);
            return false;
          }
          field.push_back(options_.quote);
        } else {
          quoted = false;
          after_quote = true;
        }
      } else {
        field.push_back(ch);
      }
    } else if (ch == options_.delimiter || ch == '\n' || ch == '\r') {
      if (!valid_utf8(field)) {
        error = "Invalid UTF-8 in CSV record " + std::to_string(record_number_ + 1);
        return false;
      }
      values.push_back(field);
      quoted_fields_.push_back(field_quoted);
      if (values.size() > options_.max_columns) {
        error = "CSV column limit exceeded at record " + std::to_string(record_number_ + 1);
        return false;
      }
      field.clear();
      after_quote = false;
      at_field_start = true;
      field_quoted = false;
      if (ch != options_.delimiter) {
        if (ch == '\r' && stream_.peek() == '\n') stream_.get();
        if (record_number_ == 0) strip_bom(values);
        ++record_number_;
        return true;
      }
    } else if (ch == options_.quote && at_field_start) {
      quoted = true;
      at_field_start = false;
      field_quoted = true;
    } else if (after_quote || ch == options_.quote) {
      error = "Invalid CSV quote at record " + std::to_string(record_number_ + 1);
      return false;
    } else {
      field.push_back(ch);
      at_field_start = false;
    }
  }
}

void CsvReader::initialize_columns(const std::vector<std::string> &first)
{
  std::unordered_set<std::string> seen;
  columns_.reserve(first.size());
  for (size_t i = 0; i < first.size(); ++i) {
    std::string name = options_.header && !first[i].empty() ? first[i] :
                       "column" + std::to_string(i + 1);
    const std::string base = name;
    size_t duplicate = 2;
    while (!seen.insert(ascii_lower(name)).second) {
      name = base + "__" + std::to_string(duplicate++);
    }
    columns_.push_back({name, CsvType::NULL_TYPE});
  }
}

bool CsvReader::is_null_value(const std::string &value, bool quoted) const
{
  return !quoted && (value.empty() || (!options_.null_text.empty() && value == options_.null_text));
}

bool CsvReader::infer_schema(std::string &error)
{
  error.clear();
  if (!check_unchanged(error) || !open_stream(error)) return false;
  columns_.clear();
  schema_ready_ = false;
  std::vector<std::string> values;
  bool end = false;
  if (!read_record(values, end, error)) return false;
  if (end) {
    error = "CSV file is empty";
    return false;
  }
  initialize_columns(values);
  if (!options_.header) {
    for (size_t i = 0; i < values.size(); ++i) {
      columns_[i].type = is_null_value(values[i], quoted_fields_[i]) ?
                         CsvType::NULL_TYPE :
                         (values[i].empty() ? CsvType::VARCHAR : classify_csv_value(values[i]));
    }
  }
  while (read_record(values, end, error) && !end) {
    if (values.size() != columns_.size()) {
      error = "CSV field count mismatch at record " + std::to_string(record_number_);
      return false;
    }
    for (size_t i = 0; i < values.size(); ++i) {
      if (!is_null_value(values[i], quoted_fields_[i])) {
        columns_[i].type = widen_csv_type(columns_[i].type,
            values[i].empty() ? CsvType::VARCHAR : classify_csv_value(values[i]));
      }
    }
  }
  if (!error.empty() || !check_unchanged(error)) return false;
  schema_ready_ = true;
  return rewind(error);
}

bool CsvReader::rewind(std::string &error)
{
  if (!schema_ready_) {
    error = "CSV schema has not been inferred";
    return false;
  }
  if (!check_unchanged(error) || !open_stream(error)) return false;
  if (options_.header) {
    std::vector<std::string> ignored;
    bool end = false;
    if (!read_record(ignored, end, error) || end) {
      if (end) error = "CSV header disappeared";
      return false;
    }
  }
  return true;
}

bool CsvReader::next(std::vector<std::string> &values, std::vector<bool> &is_null,
                     bool &end, std::string &error)
{
  error.clear();
  if (!schema_ready_) {
    error = "CSV schema has not been inferred";
    return false;
  }
  if (!check_unchanged(error) || !read_record(values, end, error)) return false;
  if (!end && values.size() != columns_.size()) {
    error = "CSV field count mismatch at record " + std::to_string(record_number_);
    return false;
  }
  is_null.assign(values.size(), false);
  for (size_t i = 0; i < values.size(); ++i) is_null[i] = is_null_value(values[i], quoted_fields_[i]);
  return true;
}

} // namespace file_sql
} // namespace sql
} // namespace oceanbase
