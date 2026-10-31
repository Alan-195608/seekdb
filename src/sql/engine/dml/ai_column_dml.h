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
#include <stdint.h>
#include "lib/container/ob_iarray.h"
#include "lib/string/ob_string.h"
namespace oceanbase { namespace sql {
class ObTableModifyOp;
class ObExecContext;
bool ai_column_internal_sql_active();
bool ai_column_retry_active();
class AiColumnInternalSQLGuard {
public:
  AiColumnInternalSQLGuard();
  ~AiColumnInternalSQLGuard();
  AiColumnInternalSQLGuard(const AiColumnInternalSQLGuard &) = delete;
  AiColumnInternalSQLGuard &operator=(const AiColumnInternalSQLGuard &) = delete;
};
class AiColumnRetryGuard {
public:
  AiColumnRetryGuard(uint64_t column_id, const common::ObString &predicate);
  ~AiColumnRetryGuard();
private:
  uint64_t previous_;
  common::ObString previous_predicate_;
};
int check_ai_column_strict_read(ObExecContext &ctx, uint64_t table_id,
                                const common::ObIArray<uint64_t> &column_ids);
int synchronize_ai_columns(ObTableModifyOp &op, ObExecContext &ctx, uint64_t table_id);
} }
