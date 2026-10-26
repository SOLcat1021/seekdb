#include "sql/engine/basic/ob_file_sql_csv_reader.h"

#include <cassert>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

using oceanbase::sql::file_sql::CsvOptions;
using oceanbase::sql::file_sql::CsvReader;
using oceanbase::sql::file_sql::CsvType;
using oceanbase::sql::file_sql::classify_csv_value;
using oceanbase::sql::file_sql::widen_csv_type;

static void write_file(const std::string &path, const std::string &data)
{
  std::ofstream file(path.c_str(), std::ios::binary | std::ios::trunc);
  assert(file.good());
  file << data;
  assert(file.good());
}

int main(int argc, char **argv)
{
  assert(argc == 2);
  const std::string path = argv[1];
  std::string error;
  CsvReader reader;
  std::vector<std::string> values;
  std::vector<bool> is_null;
  bool end = false;

  write_file(path, "id,id,note,score,day,created,all_null\r\n"
                   "1,101,\"hello,\r\nworld\",98.5,2026-08-01,2026-08-01 10:11:12,\r\n"
                   "2,102,\"say \"\"hi\"\"\",87,2026-08-02,2026-08-02T12:13:14,\r\n");
  assert(reader.open(path, CsvOptions(), error));
  assert(reader.infer_schema(error));
  const auto &cols = reader.columns();
  assert(cols.size() == 7);
  assert(cols[0].name == "id" && cols[1].name == "id__2");
  assert(cols[0].type == CsvType::BIGINT && cols[3].type == CsvType::DOUBLE);
  assert(cols[4].type == CsvType::DATE && cols[5].type == CsvType::DATETIME);
  assert(cols[6].type == CsvType::NULL_TYPE);
  assert(reader.next(values, is_null, end, error) && !end);
  assert(values[2] == "hello,\r\nworld" && is_null[6]);
  assert(reader.next(values, is_null, end, error) && !end);
  assert(values[2] == "say \"hi\"");
  assert(reader.next(values, is_null, end, error) && end);
  assert(reader.rewind(error));
  assert(reader.next(values, is_null, end, error) && values[0] == "1");

  write_file(path, "id,id__2,id,empty\n1,2,3,\"\"\n");
  assert(reader.open(path, CsvOptions(), error));
  assert(reader.infer_schema(error));
  assert(reader.columns()[2].name == "id__3");
  assert(reader.columns()[3].type == CsvType::VARCHAR);
  assert(reader.next(values, is_null, end, error) && !is_null[3]);
  write_file(path, "id\n999999999\n");
  assert(!reader.next(values, is_null, end, error) && error.find("changed") != std::string::npos);

  write_file(path, "\xEF\xBB\xBFid,name\n1,\xE9\x99\x88\n");
  assert(reader.open(path, CsvOptions(), error));
  assert(reader.infer_schema(error));
  assert(reader.columns()[0].name == "id");
  assert(reader.next(values, is_null, end, error) && values[1] == "\xE9\x99\x88");
  write_file(path, "name\n\xC0\x80\n");
  assert(reader.open(path, CsvOptions(), error));
  assert(!reader.infer_schema(error) && error.find("UTF-8") != std::string::npos);

  assert(classify_csv_value("9223372036854775807") == CsvType::BIGINT);
  assert(classify_csv_value("9223372036854775808") == CsvType::DOUBLE);
  assert(classify_csv_value("2026-02-29") == CsvType::VARCHAR);
  assert(classify_csv_value("2024-02-29") == CsvType::DATE);
  assert(widen_csv_type(CsvType::DATE, CsvType::DATETIME) == CsvType::DATETIME);
  assert(widen_csv_type(CsvType::BOOLEAN, CsvType::BIGINT) == CsvType::VARCHAR);

  write_file(path, "a,b\n1\n");
  assert(reader.open(path, CsvOptions(), error));
  assert(!reader.infer_schema(error) && error.find("field count") != std::string::npos);
  write_file(path, "a\n\"unterminated\n");
  assert(reader.open(path, CsvOptions(), error));
  assert(!reader.infer_schema(error) && error.find("Unterminated") != std::string::npos);
  write_file(path, "a\n1234\n");
  CsvOptions limited;
  limited.max_record_bytes = 3;
  assert(reader.open(path, limited, error));
  assert(!reader.infer_schema(error) && error.find("size limit") != std::string::npos);

  std::remove(path.c_str());
  return 0;
}
