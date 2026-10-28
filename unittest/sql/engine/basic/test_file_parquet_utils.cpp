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

#include <cassert>
#include <string>
#include <vector>

using oceanbase::sql::file_sql::CsvType;
using oceanbase::sql::file_sql::ParquetReader;

int main(int argc, char **argv)
{
  assert(argc == 2);
  std::string error;
  ParquetReader reader;
  assert(reader.open(argv[1], error));
  assert(reader.infer_schema(error));
  assert(reader.columns().size() == 1);
  assert(reader.columns()[0].name == "c1");
  assert(reader.columns()[0].type == CsvType::BIGINT);
  std::vector<std::string> values;
  std::vector<bool> is_null;
  bool end = false;
  for (int expected = 0; expected < 7; ++expected) {
    assert(reader.next(values, is_null, end, error) && !end);
    assert(!is_null[0] && values[0] == std::to_string(expected));
  }
  assert(reader.next(values, is_null, end, error) && end);
  assert(reader.rewind(error));
  assert(reader.next(values, is_null, end, error) && values[0] == "0");
  return 0;
}
