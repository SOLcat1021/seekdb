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

#ifndef OCEANBASE_FILE_SQL_PARQUET_H_
#define OCEANBASE_FILE_SQL_PARQUET_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct FileSqlParquetReader FileSqlParquetReader;

// Type values match file_sql::CsvType, except 0 denotes NULL_TYPE.
FileSqlParquetReader *file_sql_parquet_open(const uint8_t *path, size_t path_len,
                                           char *error, size_t error_capacity);
void file_sql_parquet_close(FileSqlParquetReader *reader);
size_t file_sql_parquet_column_count(const FileSqlParquetReader *reader);
size_t file_sql_parquet_column_name(const FileSqlParquetReader *reader, size_t column,
                                    char *output, size_t capacity);
int32_t file_sql_parquet_column_type(const FileSqlParquetReader *reader, size_t column);
int32_t file_sql_parquet_column_nullable(const FileSqlParquetReader *reader, size_t column);
// 1 = row, 0 = end, -1 = decode error.
int32_t file_sql_parquet_next(FileSqlParquetReader *reader);
// Cell pointers remain valid until the next call to file_sql_parquet_next or close.
int32_t file_sql_parquet_cell(const FileSqlParquetReader *reader, size_t column,
                              const uint8_t **data, size_t *length, int32_t *is_null);
size_t file_sql_parquet_error(const FileSqlParquetReader *reader,
                              char *output, size_t capacity);

#ifdef __cplusplus
}
#endif
#endif
