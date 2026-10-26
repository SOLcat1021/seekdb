#include "sql/engine/basic/ob_file_sql_jsonl_reader.h"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using oceanbase::sql::file_sql::CsvType;
using oceanbase::sql::file_sql::JsonlReader;

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
  JsonlReader reader;
  std::vector<std::string> values;
  std::vector<bool> is_null;
  bool end = false;

  write_file(path, "{\"id\":1,\"name\":\"Alice\",\"score\":98.5,\"active\":true}\n"
                   "{\"id\":2,\"name\":\"Bob\",\"active\":false,\"day\":\"2026-08-02\"}\r\n"
                   "{\"id\":3,\"score\":90,\"note\":null}\n");
  assert(reader.open(path, error));
  assert(reader.infer_schema(error));
  assert(reader.columns().size() == 6);
  assert(reader.columns()[0].type == CsvType::BIGINT);
  assert(reader.columns()[2].type == CsvType::DOUBLE);
  assert(reader.columns()[4].type == CsvType::DATE);
  assert(reader.next(values, is_null, end, error) && !end);
  assert(values[0] == "1" && values[1] == "Alice" && is_null[4]);
  assert(reader.next(values, is_null, end, error) && !end);
  assert(values[4] == "2026-08-02" && is_null[2]);
  assert(reader.next(values, is_null, end, error) && is_null[5]);
  assert(reader.next(values, is_null, end, error) && end);
  assert(reader.rewind(error));
  assert(reader.next(values, is_null, end, error) && values[0] == "1");
  write_file(path, "{\"changed\":999999999}\n");
  assert(!reader.next(values, is_null, end, error) && error.find("changed") != std::string::npos);

  write_file(path, "{\"ok\":1,\"nested\":{\"bad\":2}}\n");
  assert(reader.open(path, error));
  assert(!reader.infer_schema(error) && error.find("Nested") != std::string::npos);
  write_file(path, "{\"a\":1,\"A\":2}\n");
  assert(reader.open(path, error));
  assert(!reader.infer_schema(error) && error.find("Duplicate") != std::string::npos);
  write_file(path, "{broken}\n");
  assert(reader.open(path, error));
  assert(!reader.infer_schema(error) && error.find("Invalid JSON") != std::string::npos);

  const std::string unicode_path = path + "-\xE9\x99\x88.jsonl";
  {
    const std::u8string utf8(unicode_path.begin(), unicode_path.end());
    std::ofstream file(std::filesystem::path(utf8), std::ios::binary);
    file << "{\"id\":1}\n";
    assert(file.good());
  }
  assert(reader.open(unicode_path, error));
  assert(reader.infer_schema(error));
  assert(reader.next(values, is_null, end, error) && values[0] == "1");
  reader.close();
  std::filesystem::remove(std::filesystem::path(
      std::u8string(unicode_path.begin(), unicode_path.end())));

  std::remove(path.c_str());
  return 0;
}
