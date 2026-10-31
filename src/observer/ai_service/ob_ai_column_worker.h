/*
 * Copyright (c) 2026 OceanBase.
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

#pragma once
#include "lib/task/ob_timer.h"
#include "common/mysqlclient/ob_mysql_proxy.h"
#include <map>
#include <string>
#include <vector>
namespace oceanbase { namespace observer {
const char *ai_column_result_hint();
class AiColumnWorker : public common::ObTimerTask {
public:
  AiColumnWorker();
  int start(common::ObMySQLProxy &proxy);
  void stop(); void wait(); void destroy();
  void runTimerTask() override;
private:
  using Row = std::map<std::string, std::string>;
  using Rows = std::vector<Row>;
  int read(common::ObISQLClient &client, const std::string &sql,
           const std::vector<std::string> &names, Rows &rows);
  int write(common::ObISQLClient &client, const std::string &sql, int64_t &affected);
  int process_one();
  int publish(const std::string &table, const std::string &request);
  int metadata(const std::string &table, const std::string &column, Row &row);
  static std::string quote(const std::string &value);
  static std::string literal(const std::string &value);
  common::ObTimer timer_;
  common::ObMySQLProxy *proxy_;
  int64_t owner_;
};
} }
