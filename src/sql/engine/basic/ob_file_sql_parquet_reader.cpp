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

#include "sql/engine/basic/ob_file_sql_parquet_reader.h"

#include <filesystem>
#include <sys/stat.h>

namespace oceanbase {
namespace sql {
namespace file_sql {
namespace {

#ifdef _WIN32
std::filesystem::path path_from_utf8(const std::string &bytes)
{
  return std::filesystem::path(std::u8string(bytes.begin(), bytes.end()));
}

using FileStat = struct _stat64;
bool regular_file_stat(const std::string &path, FileStat &info)
{
  return _wstat64(path_from_utf8(path).c_str(), &info) == 0 &&
         (info.st_mode & _S_IFMT) == _S_IFREG;
}
#else
using FileStat = struct stat;
bool regular_file_stat(const std::string &path, FileStat &info)
{
  return ::stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode);
}
#endif

} // namespace

bool ParquetReader::capture_identity(std::string &error)
{
  FileStat info;
  if (!regular_file_stat(path_, info)) {
    error = "Parquet path is not a readable regular file: " + path_;
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

bool ParquetReader::check_unchanged(std::string &error) const
{
  FileStat info;
  if (!regular_file_stat(path_, info) ||
      static_cast<int64_t>(info.st_size) != file_size_ ||
      static_cast<uint64_t>(info.st_dev) != file_device_ ||
      static_cast<uint64_t>(info.st_ino) != file_inode_
#ifndef _WIN32
      || static_cast<int64_t>(info.st_mtime) * 1000000000 + info.st_mtim.tv_nsec != file_mtime_
#else
      || static_cast<int64_t>(info.st_mtime) != file_mtime_
#endif
      ) {
    error = "Parquet file changed during scan: " + path_;
    return false;
  }
  return true;
}

bool ParquetReader::open(const std::string &path, std::string &error)
{
  close();
  error.clear();
  path_ = path;
  if (path.empty() || !capture_identity(error)) return false;
  char message[512] = {0};
  reader_ = file_sql_parquet_open(reinterpret_cast<const uint8_t *>(path.data()),
                                  path.size(), message, sizeof(message));
  if (reader_ == nullptr) {
    error = message;
    return false;
  }
  const size_t count = file_sql_parquet_column_count(reader_);
  if (count == 0 || count > 4096) {
    error = "Parquet column count exceeds supported range";
    close();
    return false;
  }
  columns_.reserve(count);
  for (size_t index = 0; index < count; ++index) {
    const size_t length = file_sql_parquet_column_name(reader_, index, nullptr, 0);
    const int32_t type = file_sql_parquet_column_type(reader_, index);
    if (length == 0 || length > 1024 || type < 0 || type > 6) {
      error = "Invalid Parquet column metadata";
      close();
      return false;
    }
    std::string name(length + 1, '\0');
    file_sql_parquet_column_name(reader_, index, name.data(), name.size());
    name.resize(length);
    const int32_t nullable = file_sql_parquet_column_nullable(reader_, index);
    if (nullable < 0) {
      error = "Invalid Parquet nullability metadata";
      close();
      return false;
    }
    columns_.push_back({name, static_cast<CsvType>(type), nullable != 0});
  }
  if (!check_unchanged(error)) {
    close();
    return false;
  }
  return true;
}

bool ParquetReader::infer_schema(std::string &error) const
{
  if (reader_ == nullptr) {
    error = "Parquet reader is not open";
    return false;
  }
  return check_unchanged(error);
}

bool ParquetReader::next(std::vector<std::string> &values, std::vector<bool> &is_null,
                         bool &end, std::string &error)
{
  error.clear();
  end = false;
  if (reader_ == nullptr || !check_unchanged(error)) return false;
  const int32_t status = file_sql_parquet_next(reader_);
  if (status == 0) {
    end = true;
    return true;
  } else if (status != 1) {
    char message[512] = {0};
    file_sql_parquet_error(reader_, message, sizeof(message));
    error = message;
    return false;
  }
  values.assign(columns_.size(), std::string());
  is_null.assign(columns_.size(), true);
  size_t row_bytes = 0;
  for (size_t index = 0; index < columns_.size(); ++index) {
    const uint8_t *data = nullptr;
    size_t length = 0;
    int32_t null_value = 0;
    if (file_sql_parquet_cell(reader_, index, &data, &length, &null_value) != 0 ||
        length > 16 * 1024 * 1024 - row_bytes) {
      error = "Parquet row exceeds 16 MiB limit";
      return false;
    }
    is_null[index] = null_value != 0;
    row_bytes += length;
    if (length > 0) values[index].assign(reinterpret_cast<const char *>(data), length);
  }
  return check_unchanged(error);
}

bool ParquetReader::rewind(std::string &error)
{
  if (!check_unchanged(error)) return false;
  const int64_t previous_size = file_size_;
  const int64_t previous_mtime = file_mtime_;
  const uint64_t previous_device = file_device_;
  const uint64_t previous_inode = file_inode_;
  const std::string path = path_;
  if (!open(path, error)) return false;
  if (file_size_ != previous_size || file_mtime_ != previous_mtime ||
      file_device_ != previous_device || file_inode_ != previous_inode) {
    error = "Parquet file changed during rescan";
    close();
    return false;
  }
  return true;
}

void ParquetReader::close()
{
  if (reader_ != nullptr) file_sql_parquet_close(reader_);
  reader_ = nullptr;
  columns_.clear();
}

} // namespace file_sql
} // namespace sql
} // namespace oceanbase
