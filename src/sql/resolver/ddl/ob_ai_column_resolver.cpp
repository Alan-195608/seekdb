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

#define USING_LOG_PREFIX SQL_RESV

#include "sql/resolver/ddl/ob_ai_column_resolver.h"
#include "sql/engine/expr/ob_expr_operator_factory.h"
#include "query/api/query/vector/ob_vector_index_util.h"
#include "share/ai_service/ob_ai_column_proxy.h"
#include "share/ai_service/ob_ai_service_struct.h"
#include "share/schema/ob_ai_model_mgr.h"
#include "share/schema/ob_table_schema.h"
#include "lib/oblog/ob_log_module.h"

namespace oceanbase
{
namespace sql
{

using namespace oceanbase::common;
using namespace oceanbase::share;
using namespace oceanbase::share::schema;

namespace
{

/**
 * The AI functions an AI column may use.
 */
struct ObAiColumnFuncInfo
{
  const char *name_;
  bool is_embedding_;
};

const ObAiColumnFuncInfo ob_ai_column_funcs[] = {
  {"ai_complete", false},
  {"ai_embed",    true},
};

const ObAiColumnFuncInfo *find_ai_column_func(const ObString &func_name)
{
  const ObAiColumnFuncInfo *found = NULL;
  const int64_t cnt = sizeof(ob_ai_column_funcs) / sizeof(ob_ai_column_funcs[0]);
  for (int64_t i = 0; i < cnt; ++i) {
    if (0 == func_name.case_compare(ob_ai_column_funcs[i].name_)) {
      found = &ob_ai_column_funcs[i];
      break;
    }
  }
  return found;
}

/**
 * Collect the column names an expression refers to.
 *
 * The AI column expression may only refer to ordinary columns of the table, so
 * the dependency set is what decides whether a change invalidates the result.
 */
int collect_column_refs(ParseNode *node,
                         common::ObIArray<common::ObString> &column_names)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(node)) {
    // nothing to collect
  } else if (T_COLUMN_REF == node->type_ && 3 == node->num_child_) {
    ParseNode *col_node = node->children_[2];
    if (OB_NOT_NULL(col_node) && col_node->str_len_ > 0) {
      if (OB_FAIL(column_names.push_back(ObString(col_node->str_len_, col_node->str_value_)))) {
      }
    }
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < node->num_child_; ++i) {
      if (OB_FAIL(collect_column_refs(node->children_[i], column_names))) {
      }
    }
  }
  return ret;
}

/**
 * Read the name of a function call node.
 *
 * A function call is parsed as T_FUN_SYS whose first child carries the function
 * name and whose second child carries the argument list.
 */
int get_func_call_info(ParseNode *func_node,
                       ObString &func_name,
                       ParseNode *&param_list)
{
  int ret = OB_SUCCESS;
  param_list = NULL;
  if (OB_ISNULL(func_node) || T_FUN_SYS != func_node->type_ || 1 > func_node->num_child_
      || OB_ISNULL(func_node->children_) || OB_ISNULL(func_node->children_[0])) {
    ret = OB_AI_COLUMN_INVALID_DEFINITION;
    LOG_USER_ERROR(OB_AI_COLUMN_INVALID_DEFINITION,
                   "the AI column expression must call ai_complete() or ai_embed()");
    SQL_RESV_LOG(WARN, "ai column expression is not a function call", K(ret));
  } else {
    ParseNode *name_node = func_node->children_[0];
    func_name.assign_ptr(name_node->str_value_, name_node->str_len_);
    if (2 == func_node->num_child_ && OB_NOT_NULL(func_node->children_[1])) {
      param_list = func_node->children_[1];
    }
  }
  return ret;
}

/**
 * Read the model name from the first argument of an AI function call.
 */
int get_ai_func_model_name(ParseNode *param_list,
                            ObString &model_name,
                            int64_t &declared_dim)
{
  int ret = OB_SUCCESS;
  declared_dim = 0;
  if (OB_ISNULL(param_list) || T_EXPR_LIST != param_list->type_ || 1 > param_list->num_child_) {
    ret = OB_AI_COLUMN_INVALID_DEFINITION;
    LOG_USER_ERROR(OB_AI_COLUMN_INVALID_DEFINITION,
                   "the AI column expression must call ai_complete() or ai_embed()");
  } else {
    ParseNode *model_node = param_list->children_[0];
    if (OB_ISNULL(model_node) || T_VARCHAR != model_node->type_) {
      ret = OB_AI_COLUMN_INVALID_DEFINITION;
      LOG_USER_ERROR(OB_AI_COLUMN_INVALID_DEFINITION,
                     "the AI column model name must be a string literal");
      SQL_RESV_LOG(WARN, "ai column model name is not a literal", K(ret));
    } else {
      model_name.assign_ptr(model_node->str_value_, model_node->str_len_);
      // ai_embed() accepts an explicit dimension as its third argument.
      if (3 == param_list->num_child_) {
        ParseNode *dim_node = param_list->children_[2];
        if (OB_ISNULL(dim_node) || T_INT != dim_node->type_) {
          ret = OB_AI_COLUMN_INVALID_DEFINITION;
          LOG_USER_ERROR(OB_AI_COLUMN_INVALID_DEFINITION,
                         "the AI column dimension must be an integer literal");
        } else {
          declared_dim = dim_node->value_;
        }
      }
    }
  }
  return ret;
}

/**
 * Read the dimension of a vector column.
 *
 * `ObVectorIndexUtil::get_vector_dim_from_extend_type_info` reports every column
 * that is not a vector column as an error and logs a user error, so the extended
 * type info is inspected first: only a column whose extended type is VECTOR has a
 * dimension.
 */
int get_ai_column_vector_dim(const ObColumnSchemaV2 &column, int64_t &vector_dim)
{
  int ret = OB_SUCCESS;
  vector_dim = 0;
  const common::ObIArray<common::ObString> &extended_type_info = column.get_extended_type_info();
  if (1 == extended_type_info.count()) {
    ObString type_info = extended_type_info.at(0);
    ObString type_str = type_info.split_on('(').trim();
    if (0 == type_str.compare("VECTOR")) {
      if (OB_FAIL(ObVectorIndexUtil::get_vector_dim_from_extend_type_info(
                    extended_type_info, vector_dim))) {
        ret = OB_SUCCESS;
        vector_dim = 0;
      }
    }
  }
  return ret;
}

/**
 * Check that the dimension of an embedding result matches the vector column.
 */
int check_vector_dimension(const ObColumnSchemaV2 &column,
                            int64_t declared_dim,
                            int64_t &column_dim)
{
  int ret = OB_SUCCESS;
  column_dim = 0;
  if (OB_FAIL(get_ai_column_vector_dim(column, column_dim))) {
  } else if (0 == column_dim) {
    // Not a vector column: nothing to check.
  } else if (declared_dim > 0 && declared_dim != column_dim) {
    ret = OB_ERR_INVALID_VECTOR_DIM;
    LOG_USER_ERROR(OB_ERR_INVALID_VECTOR_DIM, column_dim, declared_dim);
    SQL_RESV_LOG(WARN, "ai column vector dimension mismatch",
                 K(ret), K(column_dim), K(declared_dim));
  }
  return ret;
}



} // namespace

int ObAiColumnResolver::collect_ai_column_refs(ParseNode *expr_node,
                                                const ObString &ai_column_name,
                                                ObIArray<ObAiColumnRef> &refs)
{
  int ret = OB_SUCCESS;
  ObSEArray<ObString, 8> column_names;
  if (OB_FAIL(collect_column_refs(expr_node, column_names))) {
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < column_names.count(); ++i) {
      ObAiColumnRef ref;
      ref.ai_column_name_ = ai_column_name;
      ref.ref_column_name_ = column_names.at(i);
      if (OB_FAIL(refs.push_back(ref))) {
        SQL_RESV_LOG(WARN, "failed to record the ai column reference", K(ret), K(ref));
      }
    }
  }
  return ret;
}

int ObAiColumnResolver::check_ai_column_cross_dependency(
    const ObIArray<ObString> &ai_column_names,
    const ObIArray<ObAiColumnRef> &refs)
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < refs.count(); ++i) {
    const ObAiColumnRef &ref = refs.at(i);
    for (int64_t j = 0; OB_SUCC(ret) && j < ai_column_names.count(); ++j) {
      if (0 == ref.ref_column_name_.case_compare(ai_column_names.at(j))) {
        // An AI column must not depend on another AI column of the same table: the
        // generation order between them would be undefined. The same check rejects a
        // column that refers to itself.
        ret = OB_AI_COLUMN_DEPENDENCY_NOT_ALLOWED;
        LOG_USER_ERROR(OB_AI_COLUMN_DEPENDENCY_NOT_ALLOWED);
        SQL_RESV_LOG(WARN, "ai column depends on another ai column", K(ret), K(ref));
        break;
      }
    }
  }
  return ret;
}

int ObAiColumnResolver::get_persisted_ai_column_names(ObIAllocator &allocator,
                                                       const uint64_t table_id,
                                                       ObIArray<ObString> &ai_column_names)
{
  int ret = OB_SUCCESS;
  ai_column_names.reset();
  common::ObMySQLProxy *sql_proxy = GCTX.sql_proxy_;
  ObArenaAllocator tmp_allocator("AiColNames");
  ObSArray<ObAiColumnDef> persisted_defs;
  if (OB_ISNULL(sql_proxy)) {
    ret = OB_ERR_UNEXPECTED;
    SQL_RESV_LOG(WARN, "the sql proxy is not ready", K(ret), K(table_id));
  } else if (OB_FAIL(ObAiColumnProxy::select_ai_columns(tmp_allocator, *sql_proxy, table_id,
                                                        persisted_defs))) {
    SQL_RESV_LOG(WARN, "failed to read the ai column metadata", K(ret), K(table_id));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < persisted_defs.count(); ++i) {
      // The names outlive this call, so they are copied into the allocator of the
      // caller instead of pointing into the temporary one.
      ObString column_name;
      if (OB_FAIL(ob_write_string(allocator, persisted_defs.at(i).get_column_name(),
                                  column_name))) {
      } else if (OB_FAIL(ai_column_names.push_back(column_name))) {
      }
    }
  }
  return ret;
}

int ObAiColumnResolver::resolve_ai_column_def(ObIAllocator &allocator,
                                               ParseNode *ai_column_node,
                                               const ObColumnSchemaV2 &column,
                                               const ObString &normalized_expr,
                                               ObAiColumnDef &ai_column_def)
{
  int ret = OB_SUCCESS;
  ObString func_name;
  ObString model_name;
  ParseNode *func_node = NULL;
  ParseNode *param_list = NULL;
  int64_t declared_dim = 0;
  int64_t max_retries = 0;

  if (OB_ISNULL(ai_column_node) || T_AI_COLUMN_DEF != ai_column_node->type_
      || 2 != ai_column_node->num_child_) {
    ret = OB_ERR_UNEXPECTED;
    SQL_RESV_LOG(WARN, "invalid ai column node", K(ret));
  } else if (OB_ISNULL(func_node = ai_column_node->children_[0])) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(get_func_call_info(func_node, func_name, param_list))) {
  } else if (OB_ISNULL(find_ai_column_func(func_name))) {
    ret = OB_AI_COLUMN_INVALID_DEFINITION;
    LOG_USER_ERROR(OB_AI_COLUMN_INVALID_DEFINITION,
                   "the AI column expression must call ai_complete() or ai_embed()");
    SQL_RESV_LOG(WARN, "unsupported ai column function", K(ret), K(func_name));
  } else if (OB_FAIL(get_ai_func_model_name(param_list, model_name, declared_dim))) {
  } else if (OB_FAIL(resolve_ai_column_options(ai_column_node->children_[1], max_retries))) {
  } else {
    // The model has to exist and its type has to match the function.
    const ObAiColumnFuncInfo *func_info = find_ai_column_func(func_name);
    ObMultiVersionSchemaService *schema_service = GCTX.schema_service_;
    ObSchemaGetterGuard guard;
    const ObAiModelSchema *ai_model_schema = NULL;
    if (OB_ISNULL(schema_service)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(schema_service->get_runtime_schema_guard(guard))) {
    } else if (OB_FAIL(guard.get_ai_model_schema(model_name, ai_model_schema))
               || OB_ISNULL(ai_model_schema)) {
      // The schema guard reports a missing model as a NULL schema, not as an error.
      ret = OB_AI_FUNC_MODEL_NOT_FOUND;
      LOG_USER_ERROR(OB_AI_FUNC_MODEL_NOT_FOUND, model_name.length(), model_name.ptr());
      SQL_RESV_LOG(WARN, "ai column model not found", K(ret), K(model_name));
    } else {
      if (func_info->is_embedding_
          && ai_model_schema->get_type() != EndpointType::DENSE_EMBEDDING) {
        ret = OB_AI_COLUMN_INVALID_DEFINITION;
        LOG_USER_ERROR(OB_AI_COLUMN_INVALID_DEFINITION,
                       "ai_embed() requires a dense embedding model");
        SQL_RESV_LOG(WARN, "ai column model type mismatch", K(ret), K(model_name));
      } else if (!func_info->is_embedding_
                 && ai_model_schema->get_type() != EndpointType::COMPLETION) {
        ret = OB_AI_COLUMN_INVALID_DEFINITION;
        LOG_USER_ERROR(OB_AI_COLUMN_INVALID_DEFINITION,
                       "ai_complete() requires a completion model");
        SQL_RESV_LOG(WARN, "ai column model type mismatch", K(ret), K(model_name));
      } else {
        int64_t column_dim = 0;
        if (OB_FAIL(check_vector_dimension(column, declared_dim, column_dim))) {
        } else {
          // Persist executable SQL only; the worker supplies the typed prompt
          // in its scoped model-call context.
          ObString expr_with_prompt;
          if (OB_FAIL(build_ai_column_normalized_expr(allocator, normalized_expr,
                                                       column, expr_with_prompt))) {
          } else {
            ai_column_def.set_allocator(&allocator);
            ai_column_def.set_max_retries(max_retries);
            ai_column_def.set_sync_mode(ObAiColumnSyncMode::ASYNC);
            ai_column_def.set_definition_version(1);
            if (OB_FAIL(ai_column_def.set_column_name(allocator, column.get_column_name_str()))) {
            } else if (OB_FAIL(ai_column_def.set_model_name(allocator, model_name))) {
            } else if (OB_FAIL(ai_column_def.set_func_type(allocator, func_name))) {
            } else if (OB_FAIL(ai_column_def.set_normalized_expr(allocator, expr_with_prompt))) {
            }
          }
        }
      }
    }
  }
  return ret;
}

int ObAiColumnResolver::resolve_ai_column_options(ParseNode *option_list_node,
                                                   int64_t &max_retries)
{
  int ret = OB_SUCCESS;
  max_retries = 0;
  if (OB_ISNULL(option_list_node)) {
    // no options
  } else if (T_LINK_NODE != option_list_node->type_ && T_AI_COLUMN_OPTION_LIST != option_list_node->type_) {
    ret = OB_AI_COLUMN_INVALID_DEFINITION;
    LOG_USER_ERROR(OB_AI_COLUMN_INVALID_DEFINITION, "invalid AI column options");
  } else {
    const bool single_option = option_list_node->type_ == T_AI_COLUMN_OPTION_LIST;
    const int64_t option_count = single_option ? 1 : option_list_node->num_child_;
    for (int64_t i = 0; OB_SUCC(ret) && i < option_count; ++i) {
      ParseNode *option_node = single_option ? option_list_node : option_list_node->children_[i];
      if (OB_ISNULL(option_node) || T_AI_COLUMN_OPTION_LIST != option_node->type_
          || 2 != option_node->num_child_) {
        ret = OB_AI_COLUMN_INVALID_DEFINITION;
        LOG_USER_ERROR(OB_AI_COLUMN_INVALID_DEFINITION, "invalid AI column option");
        break;
      }
      ParseNode *name_node = option_node->children_[0];
      ParseNode *value_node = option_node->children_[1];
      if (OB_ISNULL(name_node) || OB_ISNULL(value_node)) {
        ret = OB_AI_COLUMN_INVALID_DEFINITION;
        LOG_USER_ERROR(OB_AI_COLUMN_INVALID_DEFINITION, "invalid AI column option");
        break;
      }
      ObString option_name(name_node->str_len_, name_node->str_value_);
      if (0 == option_name.case_compare("sync_mode")) {
        if (T_VARCHAR != value_node->type_) {
          ret = OB_AI_COLUMN_INVALID_DEFINITION;
          LOG_USER_ERROR(OB_AI_COLUMN_INVALID_DEFINITION, "sync_mode must be a string literal");
        } else {
          ObString sync_mode(value_node->str_len_, value_node->str_value_);
          if (0 != sync_mode.case_compare("async")) {
            ret = OB_AI_COLUMN_INVALID_DEFINITION;
            LOG_USER_ERROR(OB_AI_COLUMN_INVALID_DEFINITION, "only sync_mode='async' is supported");
          }
        }
      } else if (0 == option_name.case_compare("max_retries")) {
        if (T_INT != value_node->type_ || value_node->value_ < 0) {
          ret = OB_AI_COLUMN_INVALID_DEFINITION;
          LOG_USER_ERROR(OB_AI_COLUMN_INVALID_DEFINITION, "max_retries must be a non-negative integer");
        } else {
          max_retries = value_node->value_;
        }
      } else {
        ret = OB_AI_COLUMN_INVALID_DEFINITION;
        LOG_USER_ERROR(OB_AI_COLUMN_INVALID_DEFINITION, "unknown AI column option");
        SQL_RESV_LOG(WARN, "unknown ai column option", K(ret), K(option_name));
      }
    }
  }
  return ret;
}

int ObAiColumnResolver::build_ai_column_prompt(ObIAllocator &allocator,
                                                const ObColumnSchemaV2 &column,
                                                ObString &prompt)
{
  int ret = OB_SUCCESS;
  prompt.reset();
  // The prompt depends on the type of the result column. The model has to be told
  // how the result has to look, otherwise the result cannot be written into the
  // column.
  int64_t vector_dim = 0;
  if (OB_FAIL(get_ai_column_vector_dim(column, vector_dim))) {
  }
  const char *prompt_text = NULL;
  if (column.is_json()) {
    prompt_text = "Return exactly one valid JSON value.";
  } else if (column.get_data_type() == ObIntType || column.get_data_type() == ObTinyIntType
             || column.get_data_type() == ObSmallIntType || column.get_data_type() == ObMediumIntType
             || column.get_data_type() == ObInt32Type) {
    prompt_text = "Return exactly one base-10 integer.";
  } else if (vector_dim > 0) {
    prompt_text = "Return exactly one dense vector.";
  }
  if (OB_ISNULL(prompt_text)) {
    // The result of the other types is stored as it is.
    ret = OB_SUCCESS;
  } else {
    const int64_t len = static_cast<int64_t>(strlen(prompt_text));
    char *buf = static_cast<char *>(allocator.alloc(len + 1));
    if (OB_ISNULL(buf)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      MEMCPY(buf, prompt_text, len);
      buf[len] = '\0';
      prompt.assign_ptr(buf, len);
    }
  }
  return ret;
}

int ObAiColumnResolver::build_ai_column_normalized_expr(ObIAllocator &allocator,
                                                         const ObString &normalized_expr,
                                                         const ObColumnSchemaV2 &column,
                                                         ObString &result_expr)
{
  int ret = OB_SUCCESS;
  ObString prompt;
  result_expr = normalized_expr;
  // Keep the declared type hint auditable in persisted metadata, but make it a
  // SQL comment so the entire normalized expression remains executable SQL.
  // The worker supplies the actual hint to the model in its scoped call context.
  if (OB_FAIL(build_ai_column_prompt(allocator, column, prompt))) {
  } else if (!prompt.empty()) {
    const int64_t size = normalized_expr.length() + prompt.length() + 7;
    char *buf = static_cast<char *>(allocator.alloc(size + 1));
    if (OB_ISNULL(buf)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      int64_t pos = 0;
      MEMCPY(buf + pos, normalized_expr.ptr(), normalized_expr.length());
      pos += normalized_expr.length();
      MEMCPY(buf + pos, " /* ", 4);
      pos += 4;
      MEMCPY(buf + pos, prompt.ptr(), prompt.length());
      pos += prompt.length();
      MEMCPY(buf + pos, " */", 3);
      pos += 3;
      buf[pos] = '\0';
      result_expr.assign_ptr(buf, pos);
    }
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
