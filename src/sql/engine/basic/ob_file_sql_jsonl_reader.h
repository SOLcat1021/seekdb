#pragma once

#include "sql/engine/basic/ob_file_sql_csv_reader.h"

#include <cstdint>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace oceanbase {
namespace sql {
namespace file_sql {

// JSONL schema is the union of keys in all rows. The second pass streams
// records and fills absent keys with NULL without retaining prior rows.
class JsonlReader {
public:
  JsonlReader() = default;
  JsonlReader(const JsonlReader &) = delete;
  JsonlReader &operator=(const JsonlReader &) = delete;

  bool open(const std::string &path, std::string &error);
  bool infer_schema(std::string &error);
  bool next(std::vector<std::string> &values, std::vector<bool> &is_null,
            bool &end, std::string &error);
  bool rewind(std::string &error);
  void close() { stream_.close(); schema_ready_ = false; }
  const std::vector<CsvColumn> &columns() const { return columns_; }
  int64_t file_size() const { return file_size_; }
  int64_t file_mtime() const { return file_mtime_; }
  uint64_t file_device() const { return file_device_; }
  uint64_t file_inode() const { return file_inode_; }

private:
  struct Field {
    std::string name;
    std::string value;
    CsvType type = CsvType::NULL_TYPE;
    bool is_null = true;
  };
  bool capture_identity(std::string &error);
  bool check_unchanged(std::string &error) const;
  bool open_stream(std::string &error);
  bool read_line(std::string &line, bool &end, std::string &error);
  bool parse_line(const std::string &line, std::vector<Field> &fields,
                  std::string &error) const;

  std::string path_;
  std::ifstream stream_;
  std::vector<CsvColumn> columns_;
  std::unordered_map<std::string, size_t> column_index_;
  uint64_t line_number_ = 0;
  int64_t file_size_ = 0;
  int64_t file_mtime_ = 0;
  uint64_t file_device_ = 0;
  uint64_t file_inode_ = 0;
  bool schema_ready_ = false;
};

} // namespace file_sql
} // namespace sql
} // namespace oceanbase
