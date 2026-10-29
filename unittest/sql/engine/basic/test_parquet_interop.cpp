/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "sql/engine/basic/file_scan_utils.h"
#include <iostream>
using namespace oceanbase::sql::file_sql;
int main(int argc, char **argv)
{
  if (argc != 2) return 2;
  Options options; options.path = argv[1]; options.root = "/tmp"; options.format = "parquet";
  Reader reader(options);
  if (!reader.infer_schema()) { std::cerr << reader.error; return 1; }
  std::vector<Cell> row;
  while (reader.next(row)) {
    for (size_t i=0; i<row.size(); ++i) {
      if (i) std::cout << '\t';
      std::cout << (row[i].null ? "NULL" : row[i].text);
    }
    std::cout << '\n';
  }
  if (!reader.error.empty()) { std::cerr << reader.error; return 1; }
  return 0;
}
