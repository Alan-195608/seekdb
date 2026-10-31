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

#include "sql/parser/parse_node.h"
#include "share/ai_service/ob_ai_column_def.h"
#include "share/schema/ob_schema_struct.h"

namespace oceanbase
{
namespace sql
{

/**
 * One column reference of one AI column expression.
 *
 * Both names point into the parse tree, which outlives the whole resolve, so the
 * reference needs no copy.
 */
struct ObAiColumnRef
{
  ObAiColumnRef() : ai_column_name_(), ref_column_name_() {}
  TO_STRING_KV(K_(ai_column_name), K_(ref_column_name));

  common::ObString ai_column_name_;
  common::ObString ref_column_name_;
};

/**
 * Resolves the `AI COLUMN (<expr>) WITH (sync_mode=..., max_retries=...)` part of a
 * column definition.
 *
 * The resolver validates that
 *  - the expression calls exactly one supported AI function (AI_COMPLETE / AI_EMBED),
 *  - the referenced model exists and its type matches the function,
 *  - a VECTOR column gets a vector whose dimension matches the column dimension,
 *  - the expression does not reference another AI column of the same table,
 * and it produces the normalized expression that is persisted in __all_ai_column.
 */
class ObAiColumnResolver
{
public:
  ObAiColumnResolver() = default;
  ~ObAiColumnResolver() = default;

  /**
   * Parse and validate the AI column definition of one column.
   *
   * @param allocator        allocator used for the strings kept in the definition
   * @param ai_column_node   the T_AI_COLUMN_DEF parse node
   * @param column           the column being defined, used for the type checks
   * @param normalized_expr  the text of the AI column expression
   * @param ai_column_def    output, filled on success
   */
  static int resolve_ai_column_def(common::ObIAllocator &allocator,
                                    ParseNode *ai_column_node,
                                    const share::schema::ObColumnSchemaV2 &column,
                                    const common::ObString &normalized_expr,
                                    share::ObAiColumnDef &ai_column_def);

  /**
   * Read the `WITH (...)` options of an AI column.
   *
   * @param option_list_node the option list parse node, may be NULL
   * @param max_retries      output, the retry budget of the AI column
   */
  static int resolve_ai_column_options(ParseNode *option_list_node,
                                        int64_t &max_retries);

  /**
   * Collect the column references of the expression of one AI column.
   *
   * The references are collected per AI column, so that the cross check can name
   * both sides of a forbidden dependency.
   *
   * @param expr_node       the expression of the AI column
   * @param ai_column_name  the name of the AI column the expression belongs to
   * @param refs            output, appended with one entry per referenced column
   */
  static int collect_ai_column_refs(ParseNode *expr_node,
                                     const common::ObString &ai_column_name,
                                     common::ObIArray<ObAiColumnRef> &refs);

  /**
   * Reject every reference that points at an AI column of the same table.
   *
   * The names of the AI columns have to be complete: an AI column added by the same
   * statement is only visible here, so the caller has to collect all the AI columns
   * of the table before calling this check.
   *
   * @param ai_column_names the names of all AI columns of the table
   * @param refs            the references collected for the AI columns being defined
   */
  static int check_ai_column_cross_dependency(
      const common::ObIArray<common::ObString> &ai_column_names,
      const common::ObIArray<ObAiColumnRef> &refs);

  /**
   * Read the names of the AI columns that __all_ai_column already holds for a table.
   *
   * The schema of an existing table does not carry its AI column definitions, so the
   * names of the AI columns an ALTER TABLE has to check against come from the
   * metadata table.
   *
   * @param allocator        allocator that owns the returned names
   * @param table_id         the table whose AI columns are read
   * @param ai_column_names  output, the names of the persisted AI columns
   */
  static int get_persisted_ai_column_names(common::ObIAllocator &allocator,
                                            const uint64_t table_id,
                                            common::ObIArray<common::ObString> &ai_column_names);

  /**
   * Build the system prompt that the model request of an AI column carries.
   *
   * The prompt depends on the type of the result column: a JSON column asks the
   * model for exactly one valid JSON value, an integer column asks for a base-10
   * integer, and a vector column asks for a vector of the column dimension.
   *
   * @param allocator   allocator used for the resulting prompt
   * @param column      the result column of the AI column
   * @param prompt      output, the prompt of the model request
   */
  static int build_ai_column_prompt(common::ObIAllocator &allocator,
                                     const share::schema::ObColumnSchemaV2 &column,
                                     common::ObString &prompt);

  /**
   * Keep the executable normalized expression of an AI column.
   *
   * The worker supplies the type prompt in its model-call context.
   *
   * @param allocator        allocator used for the resulting expression
   * @param normalized_expr  the text of the AI column expression
   * @param column           the result column of the AI column
   * @param result_expr      output, the executable SQL expression
   */
  static int build_ai_column_normalized_expr(common::ObIAllocator &allocator,
                                              const common::ObString &normalized_expr,
                                              const share::schema::ObColumnSchemaV2 &column,
                                              common::ObString &result_expr);

};

} // namespace sql
} // namespace oceanbase
