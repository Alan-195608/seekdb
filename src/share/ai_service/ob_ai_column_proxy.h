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

#pragma once

#include "share/ai_service/ob_ai_column_def.h"
#include "share/ob_dml_sql_splicer.h"
#include "lib/allocator/page_arena.h"
#include "common/mysqlclient/ob_isql_client.h"
#include "common/mysqlclient/ob_mysql_transaction.h"

namespace oceanbase
{
namespace share
{

/**
 * Persistence of the AI column metadata and of the pending generation tasks.
 *
 * The metadata of an AI column lives in __all_ai_column, the pending generation
 * requests live in __all_ai_column_task. Both are written inside the transaction
 * that caused the change, so a rollback removes the task together with the data
 * change and the model request is never issued for rolled back work.
 */
class ObAiColumnProxy
{
public:
  ObAiColumnProxy() = default;
  ~ObAiColumnProxy() = default;

  /* metadata of an AI column */

  /**
   * Insert the definition of an AI column.
   *
   * The write joins the caller's transaction so that the metadata and the table
   * schema change become visible at the same time.
   */
  static int insert_ai_column(common::ObMySQLTransaction &trans,
                               const share::ObAiColumnDef &ai_column_def);

  /**
   * Delete the definition of an AI column.
   */
  static int delete_ai_column(common::ObMySQLTransaction &trans,
                               const uint64_t table_id,
                               const uint64_t column_id);

  /**
   * Delete the definitions of all AI columns of one table.
   */
  static int delete_ai_columns_of_table(common::ObMySQLTransaction &trans,
                                         const uint64_t table_id);

  /**
   * Read the definitions of the AI columns of one table.
   *
   * The strings of the definitions are allocated by @a allocator, which has to
   * outlive them.
   */
  static int select_ai_columns(common::ObIAllocator &allocator,
                                common::ObISQLClient &sql_proxy,
                                const uint64_t table_id,
                                common::ObIArray<share::ObAiColumnDef> &ai_column_defs);

  /* pending generation tasks */

  /**
   * Insert one pending generation task.
   */
  static int insert_ai_column_task(common::ObISQLClient &trans,
                                    const share::ObAiColumnTask &task);

  /**
   * Delete the tasks of one request.
   */
  // DDL removes queued work in the same transaction as the definition.
  static int delete_ai_column_tasks_of_table(common::ObISQLClient &trans,
                                              const uint64_t table_id,
                                              const uint64_t column_id = OB_INVALID_ID);

  static int delete_ai_column_tasks(common::ObISQLClient &trans,
                                    const uint64_t table_id,
                                    const uint64_t request_id);

  /**
   * Delete the tasks of one request and one AI column.
   */
  static int delete_ai_column_task(common::ObISQLClient &trans,
                                   const uint64_t table_id,
                                   const uint64_t request_id,
                                   const uint64_t column_id);

  /**
   * Update the state of one task.
   */
  static int update_ai_column_task_state(common::ObISQLClient &trans,
                                          const uint64_t table_id,
                                          const uint64_t request_id,
                                          const uint64_t column_id,
                                          const share::ObAiColumnTaskState state,
                                          const int64_t retry_count,
                                          const int64_t eligible_ts,
                                          const common::ObString &last_error);

  /**
   * Read the tasks that are ready to be executed.
   */
  static int select_runnable_ai_column_tasks(common::ObArenaAllocator &allocator,
                                              common::ObISQLClient &sql_proxy,
                                              const int64_t limit,
                                              common::ObIArray<share::ObAiColumnTask> &tasks);
};

} // namespace observer
} // namespace oceanbase
