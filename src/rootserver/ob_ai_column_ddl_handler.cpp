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

#define USING_LOG_PREFIX RS

#include "rootserver/ob_ai_column_ddl_handler.h"
#include "share/ai_service/ob_ai_column_proxy.h"
#include "share/inner_table/ob_inner_table_schema.h"
#include "lib/oblog/ob_log_module.h"

namespace oceanbase
{
namespace rootserver
{

using namespace oceanbase::common;
using namespace oceanbase::share;
using namespace oceanbase::share::schema;

int ObAiColumnDDLHandler::build_hidden_column_name(ObIAllocator &allocator,
                                                  const ObString &ai_column_name,
                                                  const char *suffix,
                                                  ObString &hidden_name)
{
  int ret = OB_SUCCESS;
  const int64_t suffix_len = static_cast<int64_t>(strlen(suffix));
  const int64_t name_len = ai_column_name.length() + suffix_len;
  if (name_len >= OB_MAX_COLUMN_NAME_LENGTH) {
    ret = OB_ERR_TOO_LONG_IDENT;
    LOG_USER_ERROR(OB_ERR_TOO_LONG_IDENT, ai_column_name.length(), ai_column_name.ptr());
    RS_LOG(WARN, "the name of the ai column is too long for its hidden columns",
           K(ret), K(name_len));
  } else {
    char *buf = static_cast<char *>(allocator.alloc(name_len + 1));
    if (OB_ISNULL(buf)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      MEMCPY(buf, ai_column_name.ptr(), ai_column_name.length());
      MEMCPY(buf + ai_column_name.length(), suffix, suffix_len);
      buf[name_len] = '\0';
      hidden_name.assign_ptr(buf, name_len);
    }
  }
  return ret;
}

int ObAiColumnDDLHandler::fill_hidden_column(ObIAllocator &allocator,
                                              const ObString &column_name,
                                              const int64_t data_length,
                                              const uint64_t column_id,
                                              const uint64_t table_id,
                                              ObColumnSchemaV2 *hidden_column)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(hidden_column)) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    hidden_column->set_table_id(table_id);
    hidden_column->set_column_id(column_id);
    hidden_column->set_rowkey_position(0);
    hidden_column->set_index_position(0);
    hidden_column->set_not_part_key();
    hidden_column->set_is_hidden(true);
    hidden_column->set_nullable(true);
    hidden_column->set_data_type(ObVarcharType);
    hidden_column->set_data_length(data_length);
    hidden_column->set_collation_type(CS_TYPE_BINARY);
    hidden_column->set_charset_type(ObCharset::charset_type_by_coll(CS_TYPE_BINARY));
    hidden_column->set_prev_column_id(UINT64_MAX);
    hidden_column->set_next_column_id(UINT64_MAX);
    if (OB_FAIL(hidden_column->set_column_name(column_name))) {
    }
  }
  return ret;
}

int ObAiColumnDDLHandler::create_hidden_columns(ObTableSchema &table_schema)
{
  int ret = OB_SUCCESS;
  if (!table_schema.has_ai_column()) {
    // Nothing to do.
  } else {
    ObIAllocator *allocator = table_schema.get_allocator();
    if (OB_ISNULL(allocator)) {
      ret = OB_ERR_UNEXPECTED;
      RS_LOG(WARN, "the table schema has no allocator", K(ret));
    }
    const uint64_t table_id = table_schema.get_table_id();
    uint64_t next_column_id = table_schema.get_max_used_column_id() + 1;
    ObSArray<ObAiColumnDef> &ai_column_defs = table_schema.get_ai_column_defs();
    for (int64_t i = 0; OB_SUCC(ret) && i < ai_column_defs.count(); ++i) {
      ObAiColumnDef &ai_column_def = ai_column_defs.at(i);
      if (ai_column_def.has_hidden_columns()) {
        // The hidden columns of the AI column are already part of the schema: a
        // table that is redefined inherits them from the table it replaces, so
        // they must not be created a second time.
        continue;
      }
      ObString state_name;
      ObString request_name;
      ObString result_spec_name;
      ObColumnSchemaV2 state_column(allocator);
      ObColumnSchemaV2 request_column(allocator);
      ObColumnSchemaV2 result_spec_column(allocator);
      const uint64_t first_column_id = next_column_id;
      if (OB_FAIL(build_hidden_column_name(*allocator, ai_column_def.get_column_name(),
                                              "__ai_state", state_name))) {
      } else if (OB_FAIL(build_hidden_column_name(*allocator, ai_column_def.get_column_name(),
                                                     "__ai_request", request_name))) {
      } else if (OB_FAIL(build_hidden_column_name(*allocator, ai_column_def.get_column_name(),
                                                     "__ai_spec", result_spec_name))) {
      } else if (OB_FAIL(fill_hidden_column(*allocator, state_name,
                                                      STATE_COLUMN_LENGTH,
                                                      first_column_id, table_id, &state_column))) {
      } else if (OB_FAIL(fill_hidden_column(*allocator, request_name,
                                                      REQUEST_COLUMN_LENGTH,
                                                      first_column_id + 1, table_id,
                                                      &request_column))) {
      } else if (OB_FAIL(fill_hidden_column(*allocator, result_spec_name,
                                                      RESULT_SPEC_COLUMN_LENGTH,
                                                      first_column_id + 2, table_id,
                                                      &result_spec_column))) {
      } else if (OB_FAIL(table_schema.add_column(state_column))) {
      } else if (OB_FAIL(table_schema.add_column(request_column))) {
      } else if (OB_FAIL(table_schema.add_column(result_spec_column))) {
      } else {
        ai_column_def.set_state_column_id(first_column_id);
        ai_column_def.set_request_column_id(first_column_id + 1);
        ai_column_def.set_result_spec_column_id(first_column_id + 2);
        next_column_id = first_column_id + 3;
      }
    }
    if (OB_SUCC(ret)) {
      table_schema.set_max_used_column_id(next_column_id - 1);
    }
  }
  return ret;
}

int ObAiColumnDDLHandler::insert_ai_columns(ObMySQLTransaction &trans,
                                            const ObTableSchema &table_schema)
{
  int ret = OB_SUCCESS;
  const ObSArray<ObAiColumnDef> &ai_column_defs = table_schema.get_ai_column_defs();
  for (int64_t i = 0; OB_SUCC(ret) && i < ai_column_defs.count(); ++i) {
    if (OB_FAIL(insert_ai_column(trans, table_schema.get_table_id(), ai_column_defs.at(i)))) {
    }
  }
  return ret;
}

int ObAiColumnDDLHandler::insert_ai_column(ObMySQLTransaction &trans,
                                           const uint64_t table_id,
                                           const ObAiColumnDef &ai_column_def)
{
  int ret = OB_SUCCESS;
  ObAiColumnDef stored_def = ai_column_def;
  stored_def.set_table_id(table_id);
  if (OB_FAIL(ObAiColumnProxy::insert_ai_column(trans, stored_def))) {
    RS_LOG(WARN, "failed to persist the ai column metadata", K(ret), K(table_id),
           "column_name", stored_def.get_column_name());
  }
  return ret;
}

int ObAiColumnDDLHandler::load_ai_column_defs(ObISQLClient &sql_client,
                                              ObTableSchema &table_schema,
                                              ObIArray<ObAiColumnDef> *loaded_defs)
{
  int ret = OB_SUCCESS;
  ObIAllocator *allocator = table_schema.get_allocator();
  ObSArray<ObAiColumnDef> persisted_defs;
  const uint64_t table_id = table_schema.get_table_id();
  if (OB_ISNULL(allocator)) {
    ret = OB_ERR_UNEXPECTED;
    RS_LOG(WARN, "the table schema has no allocator", K(ret), K(table_id));
  } else if (OB_FAIL(ObAiColumnProxy::select_ai_columns(*allocator, sql_client, table_id,
                                                        persisted_defs))) {
    RS_LOG(WARN, "failed to read the ai column metadata", K(ret), K(table_id));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < persisted_defs.count(); ++i) {
      // The strings of a definition are held by the allocator of the table schema,
      // so appending the definition does not leave a dangling pointer behind.
      if (NULL == table_schema.get_ai_column_def(persisted_defs.at(i).get_column_id())
          && OB_FAIL(table_schema.add_ai_column_def(persisted_defs.at(i)))) {
        RS_LOG(WARN, "failed to record the ai column definition", K(ret), K(table_id));
      } else if (OB_NOT_NULL(loaded_defs)
                 && OB_FAIL(loaded_defs->push_back(persisted_defs.at(i)))) {
        RS_LOG(WARN, "failed to record the ai column definition", K(ret), K(table_id));
      }
    }
  }
  return ret;
}

int ObAiColumnDDLHandler::delete_ai_columns(ObMySQLTransaction &trans,
                                            const uint64_t table_id)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ObAiColumnProxy::delete_ai_columns_of_table(trans, table_id))) {
    RS_LOG(WARN, "failed to remove the ai column metadata", K(ret), K(table_id));
  }
  return ret;
}

int ObAiColumnDDLHandler::sync_ai_columns(ObMySQLTransaction &trans,
                                          const ObIArray<ObAiColumnDef> &orig_ai_column_defs,
                                          const ObTableSchema &new_table_schema)
{
  int ret = OB_SUCCESS;
  const uint64_t table_id = new_table_schema.get_table_id();
  for (int64_t i = 0; OB_SUCC(ret) && i < orig_ai_column_defs.count(); ++i) {
    const ObAiColumnDef &orig_ai_column_def = orig_ai_column_defs.at(i);
    if (OB_ISNULL(new_table_schema.get_ai_column_def(orig_ai_column_def.get_column_id()))) {
      // The AI column does not exist any more, so its metadata is removed inside
      // the DDL transaction and no orphan row survives the change.
      if (OB_FAIL(ObAiColumnProxy::delete_ai_column(trans, table_id,
                                                    orig_ai_column_def.get_column_id()))) {
        RS_LOG(WARN, "failed to remove the ai column metadata", K(ret), K(table_id),
               "column_name", orig_ai_column_def.get_column_name());
      }
    }
  }
  return ret;
}

} // namespace rootserver
} // namespace oceanbase
