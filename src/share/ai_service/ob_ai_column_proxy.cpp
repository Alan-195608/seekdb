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

#define USING_LOG_PREFIX SHARE

#include "share/ai_service/ob_ai_column_proxy.h"
#include "share/ob_dml_sql_splicer.h"
#include "share/inner_table/ob_inner_table_schema.h"
#include "common/mysqlclient/ob_mysql_proxy.h"
#include "lib/utility/utility.h"

namespace oceanbase
{
namespace share
{

using namespace oceanbase::common;
using namespace oceanbase::common::sqlclient;

namespace
{
/**
 * Append the value of a column that holds a string.
 *
 * The value is hex escaped so that it cannot terminate the SQL statement.
 */
int add_string_column(ObDMLSqlSplicer &sql, const char *name, const ObString &value)
{
  return sql.add_column(name, ObHexEscapeSqlStr(value));
}
} // namespace

int ObAiColumnProxy::insert_ai_column(ObMySQLTransaction &trans,
                                       const ObAiColumnDef &ai_column_def)
{
  int ret = OB_SUCCESS;
  ObDMLSqlSplicer sql;
  ObSqlString buffer;
  int64_t affected_rows = 0;
  if (OB_FAIL(sql.add_pk_column("table_id", static_cast<int64_t>(ai_column_def.get_table_id())))) {
  } else if (OB_FAIL(sql.add_pk_column("column_id", static_cast<int64_t>(ai_column_def.get_column_id())))) {
  } else if (OB_FAIL(add_string_column(sql, "column_name", ai_column_def.get_column_name()))) {
  } else if (OB_FAIL(sql.add_column("state_column_id", static_cast<int64_t>(ai_column_def.get_state_column_id())))) {
  } else if (OB_FAIL(sql.add_column("request_column_id", static_cast<int64_t>(ai_column_def.get_request_column_id())))) {
  } else if (OB_FAIL(sql.add_column("result_spec_column_id", static_cast<int64_t>(ai_column_def.get_result_spec_column_id())))) {
  } else if (OB_FAIL(add_string_column(sql, "normalized_expr", ai_column_def.get_normalized_expr()))) {
  } else if (OB_FAIL(add_string_column(sql, "model_name", ai_column_def.get_model_name()))) {
  } else if (OB_FAIL(add_string_column(sql, "func_type", ai_column_def.get_func_type()))) {
  } else if (OB_FAIL(sql.add_column("max_retries", ai_column_def.get_max_retries()))) {
  } else if (OB_FAIL(sql.add_column("sync_mode", static_cast<int64_t>(ai_column_def.get_sync_mode())))) {
  } else if (OB_FAIL(sql.add_column("definition_version", ai_column_def.get_definition_version()))) {
  } else if (OB_FAIL(sql.splice_insert_sql(OB_ALL_AI_COLUMN_TNAME, buffer))) {
  } else if (OB_FAIL(trans.write(buffer.ptr(), affected_rows))) {
  } else if (1 != affected_rows) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected affected rows for the ai column metadata", K(ret), K(affected_rows));
  }
  return ret;
}

int ObAiColumnProxy::delete_ai_column(ObMySQLTransaction &trans,
                                       const uint64_t table_id,
                                       const uint64_t column_id)
{
  int ret = OB_SUCCESS;
  ObDMLSqlSplicer sql;
  ObSqlString buffer;
  int64_t affected_rows = 0;
  if (OB_FAIL(sql.add_pk_column("table_id", static_cast<int64_t>(table_id)))) {
  } else if (OB_FAIL(sql.add_pk_column("column_id", static_cast<int64_t>(column_id)))) {
  } else if (OB_FAIL(sql.splice_delete_sql(OB_ALL_AI_COLUMN_TNAME, buffer))) {
  } else if (OB_FAIL(trans.write(buffer.ptr(), affected_rows))) {
  } else if (affected_rows > 1) {
    // The delete is scoped by the whole rowkey, so it can only match one row.
    // Matching no row is allowed: the caller removes the metadata of a column
    // that may already be gone.
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected affected rows when deleting the ai column metadata", K(ret), K(affected_rows));
  }
  if (OB_SUCC(ret)
      && OB_FAIL(delete_ai_column_tasks_of_table(trans, table_id, column_id))) {
  }
  return ret;
}

int ObAiColumnProxy::delete_ai_columns_of_table(ObMySQLTransaction &trans,
                                                const uint64_t table_id)
{
  int ret = OB_SUCCESS;
  ObDMLSqlSplicer sql;
  ObSqlString buffer;
  int64_t affected_rows = 0;
  // The delete statement uses the primary key columns of the rowkey, so the
  // table id has to be added as a primary key column.
  if (OB_FAIL(sql.add_pk_column("table_id", static_cast<int64_t>(table_id)))) {
  } else if (OB_FAIL(sql.splice_delete_sql(OB_ALL_AI_COLUMN_TNAME, buffer))) {
  } else if (OB_FAIL(trans.write(buffer.ptr(), affected_rows))) {
  }
  if (OB_SUCC(ret) && OB_FAIL(delete_ai_column_tasks_of_table(trans, table_id))) {
  }
  return ret;
}

int ObAiColumnProxy::select_ai_columns(ObIAllocator &allocator,
                                       ObISQLClient &sql_proxy,
                                       const uint64_t table_id,
                                       ObIArray<ObAiColumnDef> &ai_column_defs)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  if (OB_FAIL(sql.assign_fmt(
          "SELECT table_id, column_id, column_name, state_column_id, request_column_id, "
          "result_spec_column_id, normalized_expr, model_name, func_type, max_retries, "
          "sync_mode, definition_version FROM %s WHERE table_id = %lu",
          OB_ALL_AI_COLUMN_TNAME, table_id))) {
  } else {
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      ObMySQLResult *result = NULL;
      if (OB_FAIL(sql_proxy.read(res, sql.ptr()))) {
      } else if (OB_ISNULL(result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        while (OB_SUCC(ret)) {
          // The result reports the end of the rows with OB_ITER_END, which is not
          // an error: a table without AI columns simply has no rows here.
          int next_ret = result->next();
          if (OB_ITER_END == next_ret) {
            break;
          } else if (OB_FAIL(next_ret)) {
            ret = next_ret;
            break;
          }
          ObAiColumnDef ai_column_def;
          int64_t row_table_id = 0;
          int64_t column_id = 0;
          int64_t state_column_id = 0;
          int64_t request_column_id = 0;
          int64_t result_spec_column_id = 0;
          int64_t max_retries = 0;
          int64_t sync_mode = 0;
          int64_t definition_version = 0;
          ObString column_name;
          ObString normalized_expr;
          ObString model_name;
          ObString func_type;
          EXTRACT_INT_FIELD_MYSQL(*result, "table_id", row_table_id, uint64_t);
          EXTRACT_INT_FIELD_MYSQL(*result, "column_id", column_id, uint64_t);
          EXTRACT_INT_FIELD_MYSQL(*result, "state_column_id", state_column_id, uint64_t);
          EXTRACT_INT_FIELD_MYSQL(*result, "request_column_id", request_column_id, uint64_t);
          EXTRACT_INT_FIELD_MYSQL(*result, "result_spec_column_id", result_spec_column_id, uint64_t);
          EXTRACT_INT_FIELD_MYSQL(*result, "max_retries", max_retries, int64_t);
          EXTRACT_INT_FIELD_MYSQL(*result, "sync_mode", sync_mode, int64_t);
          EXTRACT_INT_FIELD_MYSQL(*result, "definition_version", definition_version, int64_t);
          EXTRACT_VARCHAR_FIELD_MYSQL_WITH_DEFAULT_VALUE(*result, "column_name", column_name, true, false, "");
          EXTRACT_VARCHAR_FIELD_MYSQL_WITH_DEFAULT_VALUE(*result, "normalized_expr", normalized_expr, true, false, "");
          EXTRACT_VARCHAR_FIELD_MYSQL_WITH_DEFAULT_VALUE(*result, "model_name", model_name, true, false, "");
          EXTRACT_VARCHAR_FIELD_MYSQL_WITH_DEFAULT_VALUE(*result, "func_type", func_type, true, false, "");
          if (OB_FAIL(ret)) {
          } else {
            ai_column_def.set_allocator(&allocator);
            ai_column_def.set_table_id(static_cast<uint64_t>(row_table_id));
            ai_column_def.set_column_id(static_cast<uint64_t>(column_id));
            ai_column_def.set_state_column_id(static_cast<uint64_t>(state_column_id));
            ai_column_def.set_request_column_id(static_cast<uint64_t>(request_column_id));
            ai_column_def.set_result_spec_column_id(static_cast<uint64_t>(result_spec_column_id));
            ai_column_def.set_max_retries(max_retries);
            ai_column_def.set_sync_mode(static_cast<ObAiColumnSyncMode>(sync_mode));
            ai_column_def.set_definition_version(definition_version);
            if (OB_FAIL(ai_column_def.set_column_name(allocator, column_name))) {
            } else if (OB_FAIL(ai_column_def.set_normalized_expr(allocator, normalized_expr))) {
            } else if (OB_FAIL(ai_column_def.set_model_name(allocator, model_name))) {
            } else if (OB_FAIL(ai_column_def.set_func_type(allocator, func_type))) {
            } else if (OB_FAIL(ai_column_defs.push_back(ai_column_def))) {
            }
          }
        }
      }
    }
  }
  return ret;
}

int ObAiColumnProxy::insert_ai_column_task(ObISQLClient &trans,
                                            const ObAiColumnTask &task)
{
  int ret = OB_SUCCESS;
  ObDMLSqlSplicer sql;
  ObSqlString buffer;
  int64_t affected_rows = 0;
  if (OB_FAIL(sql.add_pk_column("table_id", static_cast<int64_t>(task.get_table_id())))) {
  } else if (OB_FAIL(sql.add_pk_column("request_id", static_cast<int64_t>(task.get_request_id())))) {
  } else if (OB_FAIL(sql.add_pk_column("column_id", static_cast<int64_t>(task.get_column_id())))) {
  } else if (OB_FAIL(add_string_column(sql, "rowkey_predicate", task.get_rowkey_predicate()))) {
  } else if (OB_FAIL(sql.add_column("state", static_cast<int64_t>(task.get_state())))) {
  } else if (OB_FAIL(sql.add_column("retry_count", task.get_retry_count()))) {
  } else if (OB_FAIL(sql.add_column("eligible_ts", task.get_eligible_ts()))) {
  } else if (OB_FAIL(sql.add_column("definition_version", task.get_definition_version()))) {
  } else if (OB_FAIL(add_string_column(sql, "last_error", task.get_last_error()))) {
  } else if (OB_FAIL(sql.splice_insert_sql(OB_ALL_AI_COLUMN_TASK_TNAME, buffer))) {
  } else if (OB_FAIL(trans.write(buffer.ptr(), affected_rows))) {
  } else if (1 != affected_rows) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected affected rows for the ai column task", K(ret), K(affected_rows));
  }
  return ret;
}

int ObAiColumnProxy::delete_ai_column_tasks_of_table(ObISQLClient &trans,
                                                      const uint64_t table_id,
                                                      const uint64_t column_id)
{
  int ret = OB_SUCCESS;
  ObDMLSqlSplicer sql;
  ObSqlString buffer;
  int64_t affected_rows = 0;
  if (OB_FAIL(sql.add_pk_column("table_id", static_cast<int64_t>(table_id)))) {
  } else if (OB_INVALID_ID != column_id
             && OB_FAIL(sql.add_pk_column("column_id", static_cast<int64_t>(column_id)))) {
  } else if (OB_FAIL(sql.splice_delete_sql(OB_ALL_AI_COLUMN_TASK_TNAME, buffer))) {
  } else if (OB_FAIL(trans.write(buffer.ptr(), affected_rows))) {
  }
  return ret;
}

int ObAiColumnProxy::delete_ai_column_tasks(ObISQLClient &trans,
                                            const uint64_t table_id,
                                            const uint64_t request_id)
{
  int ret = OB_SUCCESS;
  ObDMLSqlSplicer sql;
  ObSqlString buffer;
  int64_t affected_rows = 0;
  // splice_delete_sql builds its WHERE clause from the rowkey columns only, so
  // the leading rowkey columns of the task have to be added as primary keys.
  // Without them the statement would delete every task of every table.
  if (OB_FAIL(sql.add_pk_column("table_id", static_cast<int64_t>(table_id)))) {
  } else if (OB_FAIL(sql.add_pk_column("request_id", static_cast<int64_t>(request_id)))) {
  } else if (OB_FAIL(sql.splice_delete_sql(OB_ALL_AI_COLUMN_TASK_TNAME, buffer))) {
  } else if (OB_FAIL(trans.write(buffer.ptr(), affected_rows))) {
  }
  return ret;
}

int ObAiColumnProxy::delete_ai_column_task(ObISQLClient &trans,
                                           const uint64_t table_id,
                                           const uint64_t request_id,
                                           const uint64_t column_id)
{
  int ret = OB_SUCCESS;
  ObDMLSqlSplicer sql;
  ObSqlString buffer;
  int64_t affected_rows = 0;
  if (OB_FAIL(sql.add_pk_column("table_id", static_cast<int64_t>(table_id)))) {
  } else if (OB_FAIL(sql.add_pk_column("request_id", static_cast<int64_t>(request_id)))) {
  } else if (OB_FAIL(sql.add_pk_column("column_id", static_cast<int64_t>(column_id)))) {
  } else if (OB_FAIL(sql.splice_delete_sql(OB_ALL_AI_COLUMN_TASK_TNAME, buffer))) {
  } else if (OB_FAIL(trans.write(buffer.ptr(), affected_rows))) {
  } else if (1 != affected_rows) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected affected rows when deleting the ai column task", K(ret), K(affected_rows));
  }
  return ret;
}

int ObAiColumnProxy::update_ai_column_task_state(ObISQLClient &trans,
                                                  const uint64_t table_id,
                                                  const uint64_t request_id,
                                                  const uint64_t column_id,
                                                  const ObAiColumnTaskState state,
                                                  const int64_t retry_count,
                                                  const int64_t eligible_ts,
                                                  const ObString &last_error)
{
  int ret = OB_SUCCESS;
  ObDMLSqlSplicer sql;
  ObSqlString buffer;
  int64_t affected_rows = 0;
  if (OB_FAIL(sql.add_pk_column("table_id", static_cast<int64_t>(table_id)))) {
  } else if (OB_FAIL(sql.add_pk_column("request_id", static_cast<int64_t>(request_id)))) {
  } else if (OB_FAIL(sql.add_pk_column("column_id", static_cast<int64_t>(column_id)))) {
  } else if (OB_FAIL(sql.add_column("state", static_cast<int64_t>(state)))) {
  } else if (OB_FAIL(sql.add_column("retry_count", retry_count))) {
  } else if (OB_FAIL(sql.add_column("eligible_ts", eligible_ts))) {
  } else if (OB_FAIL(add_string_column(sql, "last_error", last_error))) {
  } else if (OB_FAIL(sql.splice_update_sql(OB_ALL_AI_COLUMN_TASK_TNAME, buffer))) {
  } else if (OB_FAIL(trans.write(buffer.ptr(), affected_rows))) {
  } else if (1 != affected_rows) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected affected rows when updating the ai column task", K(ret), K(affected_rows));
  }
  return ret;
}

int ObAiColumnProxy::select_runnable_ai_column_tasks(ObArenaAllocator &allocator,
                                                      ObISQLClient &sql_proxy,
                                                      const int64_t limit,
                                                      ObIArray<ObAiColumnTask> &tasks)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  if (OB_FAIL(sql.assign_fmt(
          "SELECT table_id, request_id, column_id, rowkey_predicate, state, retry_count, "
          "eligible_ts, definition_version, last_error FROM %s WHERE state IN (0, 3) "
          "AND eligible_ts <= now(6) ORDER BY eligible_ts LIMIT %ld",
          OB_ALL_AI_COLUMN_TASK_TNAME, limit))) {
  } else {
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      ObMySQLResult *result = NULL;
      if (OB_FAIL(sql_proxy.read(res, sql.ptr()))) {
      } else if (OB_ISNULL(result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        while (OB_SUCC(ret)) {
          // The end of the rows is reported with OB_ITER_END, which is not an error.
          int next_ret = result->next();
          if (OB_ITER_END == next_ret) {
            break;
          } else if (OB_FAIL(next_ret)) {
            ret = next_ret;
            break;
          }
          ObAiColumnTask task;
          int64_t table_id = 0;
          int64_t request_id = 0;
          int64_t column_id = 0;
          int64_t state = 0;
          int64_t retry_count = 0;
          int64_t eligible_ts = 0;
          int64_t definition_version = 0;
          ObString rowkey_predicate;
          ObString last_error;
          EXTRACT_INT_FIELD_MYSQL(*result, "table_id", table_id, uint64_t);
          EXTRACT_INT_FIELD_MYSQL(*result, "request_id", request_id, uint64_t);
          EXTRACT_INT_FIELD_MYSQL(*result, "column_id", column_id, uint64_t);
          EXTRACT_INT_FIELD_MYSQL(*result, "state", state, int64_t);
          EXTRACT_INT_FIELD_MYSQL(*result, "retry_count", retry_count, int64_t);
          EXTRACT_INT_FIELD_MYSQL(*result, "eligible_ts", eligible_ts, int64_t);
          EXTRACT_INT_FIELD_MYSQL(*result, "definition_version", definition_version, int64_t);
          EXTRACT_VARCHAR_FIELD_MYSQL_WITH_DEFAULT_VALUE(*result, "rowkey_predicate", rowkey_predicate, true, false, "");
          EXTRACT_VARCHAR_FIELD_MYSQL_WITH_DEFAULT_VALUE(*result, "last_error", last_error, true, false, "");
          if (OB_FAIL(ret)) {
          } else {
            task.set_table_id(static_cast<uint64_t>(table_id));
            task.set_request_id(static_cast<uint64_t>(request_id));
            task.set_column_id(static_cast<uint64_t>(column_id));
            task.set_state(static_cast<ObAiColumnTaskState>(state));
            task.set_retry_count(retry_count);
            task.set_eligible_ts(eligible_ts);
            task.set_definition_version(definition_version);
            if (OB_FAIL(task.set_rowkey_predicate(allocator, rowkey_predicate))) {
            } else if (OB_FAIL(task.set_last_error(allocator, last_error))) {
            } else if (OB_FAIL(tasks.push_back(task))) {
            }
          }
        }
      }
    }
  }
  return ret;
}

} // namespace share
} // namespace oceanbase
