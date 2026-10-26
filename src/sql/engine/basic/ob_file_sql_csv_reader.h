#pragma once

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace oceanbase {
namespace sql {
namespace file_sql {

enum class CsvType { NULL_TYPE, BOOLEAN, BIGINT, DOUBLE, DATE, DATETIME, VARCHAR };

struct CsvColumn {
  std::string name;
  CsvType type;
};

struct CsvOptions {
  bool header = true;
  char delimiter = ',';
  char quote = '"';
  // An empty field is NULL by default. A nonempty null_text is also NULL.
  std::string null_text;
  size_t max_record_bytes = 16 * 1024 * 1024;
  size_t max_columns = 4096;
};

// A read-only, bounded-record CSV scan. Schema inference makes a complete
// first pass, but keeps only column metadata and one record in memory.
class CsvReader {
public:
  CsvReader() = default;
  ~CsvReader() = default;
  CsvReader(const CsvReader &) = delete;
  CsvReader &operator=(const CsvReader &) = delete;

  bool open(const std::string &path, const CsvOptions &options, std::string &error);
  bool infer_schema(std::string &error);
  bool next(std::vector<std::string> &values, std::vector<bool> &is_null,
            bool &end, std::string &error);
  bool rewind(std::string &error);
  void close() { stream_.close(); schema_ready_ = false; }
  const std::vector<CsvColumn> &columns() const { return columns_; }
  uint64_t record_number() const { return record_number_; }
  int64_t file_size() const { return file_size_; }
  int64_t file_mtime() const { return file_mtime_; }
  uint64_t file_device() const { return file_device_; }
  uint64_t file_inode() const { return file_inode_; }

private:
  bool read_record(std::vector<std::string> &values, bool &end,
                   std::string &error);
  bool check_unchanged(std::string &error) const;
  bool capture_identity(std::string &error);
  bool open_stream(std::string &error);
  void initialize_columns(const std::vector<std::string> &first);
  bool is_null_value(const std::string &value, bool quoted) const;

  std::string path_;
  CsvOptions options_;
  std::ifstream stream_;
  std::vector<CsvColumn> columns_;
  std::vector<bool> quoted_fields_;
  uint64_t record_number_ = 0;
  int64_t file_size_ = 0;
  int64_t file_mtime_ = 0;
  uint64_t file_device_ = 0;
  uint64_t file_inode_ = 0;
  bool schema_ready_ = false;
};

CsvType classify_csv_value(const std::string &value);
CsvType widen_csv_type(CsvType previous, CsvType next);

} // namespace file_sql
} // namespace sql
} // namespace oceanbase
