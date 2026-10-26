/* Copyright (c) 2026 OceanBase.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *     http://www.apache.org/licenses/LICENSE-2.0
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include "sql/engine/basic/ob_file_sql_csv_reader.h"
#include "file_sql_parquet.h"

#include <string>
#include <vector>

namespace oceanbase {
namespace sql {
namespace file_sql {

// A streaming wrapper around Apache Arrow's Parquet format decoder. SQL
// execution remains in seekdb; this wrapper only returns typed file cells.
class ParquetReader {
public:
  ParquetReader() = default;
  ~ParquetReader() { close(); }
  ParquetReader(const ParquetReader &) = delete;
  ParquetReader &operator=(const ParquetReader &) = delete;

  bool open(const std::string &path, std::string &error);
  bool infer_schema(std::string &error) const;
  bool next(std::vector<std::string> &values, std::vector<bool> &is_null,
            bool &end, std::string &error);
  bool rewind(std::string &error);
  void close();

  const std::vector<CsvColumn> &columns() const { return columns_; }
  int64_t file_size() const { return file_size_; }
  int64_t file_mtime() const { return file_mtime_; }
  uint64_t file_device() const { return file_device_; }
  uint64_t file_inode() const { return file_inode_; }

private:
  bool capture_identity(std::string &error);
  bool check_unchanged(std::string &error) const;

  FileSqlParquetReader *reader_ = nullptr;
  std::string path_;
  std::vector<CsvColumn> columns_;
  int64_t file_size_ = 0;
  int64_t file_mtime_ = 0;
  uint64_t file_device_ = 0;
  uint64_t file_inode_ = 0;
};

} // namespace file_sql
} // namespace sql
} // namespace oceanbase
