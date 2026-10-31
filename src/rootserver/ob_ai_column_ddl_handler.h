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

#include "share/schema/ob_table_schema.h"
#include "share/ai_service/ob_ai_column_def.h"
#include "common/mysqlclient/ob_isql_client.h"
#include "common/mysqlclient/ob_mysql_transaction.h"

namespace oceanbase
{
namespace rootserver
{

/**
 * The DDL side of an AI column.
 *
 * An AI column is declared by the user as
 *
 *   col <type> AI COLUMN ( AI_COMPLETE('model', expr) ) WITH (sync_mode='async', ...)
 *
 * The resolver validates the declaration and records it in the table schema, but
 * the physical columns of the AI column and the metadata row of __all_ai_column
 * are created by the root server, because only there the final table id and the
 * final column ids are known.
 *
 * Every AI column owns three hidden columns:
 *  - <col>__ai_state   the runtime state of the column (PENDING / READY / FAILED),
 *  - <col>__ai_request the request that produced the current result,
 *  - <col>__ai_spec    the specification of the result.
 *
 * They take part in the ordinary MVCC machinery of the table, so an invalidation
 * caused by a dependency update is visible to the transaction that caused it and
 * is rolled back with it.
 */
class ObAiColumnDDLHandler
{
public:
  ObAiColumnDDLHandler() = default;
  ~ObAiColumnDDLHandler() = default;

  /**
   * Create the hidden columns of every AI column of the table and record the
   * column ids in the AI column definitions.
   *
   * The hidden columns are appended to the table schema, so the caller has to
   * persist the table schema after this call. The names of the hidden columns
   * are allocated by the allocator of the table schema, so they live as long as
   * the schema itself.
   *
   * @param table_schema the table the AI columns belong to, modified in place
   */
  static int create_hidden_columns(share::schema::ObTableSchema &table_schema);

  /**
   * Build the name of one hidden column of an AI column.
   *
   * @param allocator     allocator used for the name
   * @param ai_column_name the name of the AI column
   * @param suffix        the suffix that marks the hidden column
   * @param hidden_name   output, the name of the hidden column
   */
  static int build_hidden_column_name(common::ObIAllocator &allocator,
                                      const common::ObString &ai_column_name,
                                      const char *suffix,
                                      common::ObString &hidden_name);

  /**
   * Fill one hidden column of an AI column.
   *
   * @param allocator   allocator used for the column
   * @param column_name the name of the hidden column
   * @param data_length the length of the hidden column
   * @param column_id   the id of the hidden column
   * @param table_id    the id of the table
   * @param hidden_column output, the hidden column
   */
  static int fill_hidden_column(common::ObIAllocator &allocator,
                                 const common::ObString &column_name,
                                 const int64_t data_length,
                                 const uint64_t column_id,
                                 const uint64_t table_id,
                                 share::schema::ObColumnSchemaV2 *hidden_column);

  /**
   * The length of the hidden state column of an AI column.
   */
  static constexpr int64_t STATE_COLUMN_LENGTH = 16;
  /**
   * The length of the hidden request column of an AI column.
   */
  static constexpr int64_t REQUEST_COLUMN_LENGTH = 64;
  /**
   * The length of the hidden result specification column of an AI column.
   */
  static constexpr int64_t RESULT_SPEC_COLUMN_LENGTH = 2048;

  /**
   * Write the metadata of every AI column of the table into __all_ai_column.
   *
   * The write joins the DDL transaction, so the metadata and the schema change
   * become visible at the same time. Only use it for a table whose AI columns are
   * all new, because the metadata table has (table_id, column_id) as its rowkey.
   *
   * @param trans        the DDL transaction
   * @param table_schema the table the AI columns belong to
   */
  static int insert_ai_columns(common::ObMySQLTransaction &trans,
                               const share::schema::ObTableSchema &table_schema);

  /**
   * Write the metadata of a single AI column into __all_ai_column.
   *
   * @param trans          the DDL transaction
   * @param table_id       the table the AI column belongs to
   * @param ai_column_def  the definition of the AI column
   */
  static int insert_ai_column(common::ObMySQLTransaction &trans,
                              const uint64_t table_id,
                              const share::ObAiColumnDef &ai_column_def);

  /**
   * Read the persisted definitions of the AI columns of the table.
   *
   * The definitions are not carried by the refreshed table schema: __all_ai_column
   * has a table id above OB_MAX_SCHEMA_FETCH_DEP_TABLE_ID, so it cannot be read
   * while the system table schemas are still being fetched. A DDL that changes the
   * AI columns of an existing table reads them here instead.
   *
   * The definitions are appended to the table schema and their strings are
   * allocated by the allocator of the table schema, so they live as long as the
   * schema itself.
   *
   * @param sql_client   the client that reads the metadata, usually the DDL transaction
   * @param table_schema the table the definitions are appended to, modified in place
   * @param loaded_defs  optional output, the definitions exactly as they were read
   */
  static int load_ai_column_defs(common::ObISQLClient &sql_client,
                                 share::schema::ObTableSchema &table_schema,
                                 common::ObIArray<share::ObAiColumnDef> *loaded_defs = NULL);

  /**
   * Delete the metadata of the AI columns of the table from __all_ai_column.
   *
   * @param trans    the DDL transaction
   * @param table_id the table whose AI columns are removed
   */
  static int delete_ai_columns(common::ObMySQLTransaction &trans,
                               const uint64_t table_id);

  /**
   * Remove the metadata of the AI columns that a table change dropped.
   *
   * The definitions of the AI columns that still exist are left untouched: an
   * online column change keeps their column ids, and a table redefinition writes
   * the metadata of the new table from scratch.
   *
   * @param trans                the DDL transaction
   * @param orig_ai_column_defs  the definitions the table had before the change
   * @param new_table_schema     the schema of the table after the change
   */
  static int sync_ai_columns(common::ObMySQLTransaction &trans,
                             const common::ObIArray<share::ObAiColumnDef> &orig_ai_column_defs,
                             const share::schema::ObTableSchema &new_table_schema);
};

} // namespace rootserver
} // namespace oceanbase
