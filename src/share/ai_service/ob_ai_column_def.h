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

#include "lib/ob_define.h"
#include "lib/string/ob_string.h"
#include "lib/container/ob_array.h"

namespace oceanbase
{
namespace share
{

/**
 * Runtime state of one AI column on one row.
 *
 * The state lives in a hidden column of the user table so that it participates in
 * the ordinary MVCC/transaction machinery: an invalidation caused by a dependency
 * update is visible to the transaction that caused it and is rolled back with it.
 */
enum class ObAiColumnState : int64_t
{
  PENDING = 0, // generation requested, no terminal result yet
  READY   = 1, // result available
  FAILED  = 2, // generation gave up after exhausting the retry budget
  MAX_STATE
};

/**
 * State of one pending AI column task in __all_ai_column_task.
 *
 * The task row is written inside the business transaction that caused the change,
 * so a rollback removes it together with the data change. The model request is only
 * issued after the transaction commits.
 *
 * The numeric values are part of the observable contract: a task that failed once
 * and is waiting for its next attempt is reported as state 3 together with
 * retry_count = 1.
 */
enum class ObAiColumnTaskState : int64_t
{
  PENDING  = 0, // waiting to be claimed by a worker
  RUNNING  = 1, // claimed by a worker
  READY    = 2, // terminal: all columns of the group succeeded
  RETRYING = 3, // a model call failed, waiting for the next attempt
  FAILED   = 4, // terminal: at least one column of the group gave up
  MAX_STATE
};

/**
 * Synchronization mode of an AI column.
 *
 * Only ASYNC is supported by this version; the value is persisted so that the
 * metadata stays forward compatible.
 */
enum class ObAiColumnSyncMode : int64_t
{
  ASYNC = 0,
  SYNC  = 1,
  MAX_MODE
};

const char *ob_ai_column_state_to_string(ObAiColumnState state);
const char *ob_ai_column_task_state_to_string(ObAiColumnTaskState state);

/**
 * The definition of one AI column.
 *
 * It is persisted in __all_ai_column and is also carried inside the table schema so
 * that the executor can resolve an AI column without an extra metadata lookup.
 */
class ObAiColumnDef
{
public:
  ObAiColumnDef()
      : allocator_(nullptr),
        table_id_(OB_INVALID_ID),
        column_id_(OB_INVALID_ID),
        state_column_id_(OB_INVALID_ID),
        request_column_id_(OB_INVALID_ID),
        result_spec_column_id_(OB_INVALID_ID),
        max_retries_(0),
        sync_mode_(ObAiColumnSyncMode::ASYNC),
        definition_version_(0)
  {}

  explicit ObAiColumnDef(common::ObIAllocator *allocator)
      : allocator_(allocator),
        table_id_(OB_INVALID_ID),
        column_id_(OB_INVALID_ID),
        state_column_id_(OB_INVALID_ID),
        request_column_id_(OB_INVALID_ID),
        result_spec_column_id_(OB_INVALID_ID),
        max_retries_(0),
        sync_mode_(ObAiColumnSyncMode::ASYNC),
        definition_version_(0)
  {}

  int assign(common::ObIAllocator &allocator, const ObAiColumnDef &other);
  void reset();
  void set_allocator(common::ObIAllocator *allocator) { allocator_ = allocator; }
  common::ObIAllocator *get_allocator() const { return allocator_; }

  /**
   * Serialize / deserialize one AI column definition.
   *
   * The definition is stored inside the table schema, so it has to follow the
   * schema's wire format. The strings are copied into the schema allocator.
   */
  int serialize(char *buf, const int64_t buf_len, int64_t &pos) const;
  int deserialize(const char *buf, const int64_t data_len, int64_t &pos);
  int64_t get_serialize_size() const;

  uint64_t get_table_id() const { return table_id_; }
  uint64_t get_column_id() const { return column_id_; }
  uint64_t get_state_column_id() const { return state_column_id_; }
  uint64_t get_request_column_id() const { return request_column_id_; }
  uint64_t get_result_spec_column_id() const { return result_spec_column_id_; }
  /**
   * Whether the three hidden columns of the AI column are known.
   */
  bool has_hidden_columns() const
  {
    return OB_INVALID_ID != state_column_id_
           && OB_INVALID_ID != request_column_id_
           && OB_INVALID_ID != result_spec_column_id_;
  }
  int64_t get_max_retries() const { return max_retries_; }
  ObAiColumnSyncMode get_sync_mode() const { return sync_mode_; }
  int64_t get_definition_version() const { return definition_version_; }

  const common::ObString &get_column_name() const { return column_name_; }
  const common::ObString &get_normalized_expr() const { return normalized_expr_; }
  const common::ObString &get_model_name() const { return model_name_; }
  const common::ObString &get_func_type() const { return func_type_; }

  void set_table_id(uint64_t id) { table_id_ = id; }
  void set_column_id(uint64_t id) { column_id_ = id; }
  void set_state_column_id(uint64_t id) { state_column_id_ = id; }
  void set_request_column_id(uint64_t id) { request_column_id_ = id; }
  void set_result_spec_column_id(uint64_t id) { result_spec_column_id_ = id; }
  void set_max_retries(int64_t v) { max_retries_ = v; }
  void set_sync_mode(ObAiColumnSyncMode mode) { sync_mode_ = mode; }
  void set_definition_version(int64_t v) { definition_version_ = v; }

  int set_column_name(common::ObIAllocator &allocator, const common::ObString &name);
  int set_normalized_expr(common::ObIAllocator &allocator, const common::ObString &expr);
  int set_model_name(common::ObIAllocator &allocator, const common::ObString &name);
  int set_func_type(common::ObIAllocator &allocator, const common::ObString &type);

  TO_STRING_KV(K_(table_id),
               K_(column_id),
               K_(state_column_id),
               K_(request_column_id),
               K_(result_spec_column_id),
               K_(max_retries),
               K_(sync_mode),
               K_(definition_version),
               K_(column_name),
               K_(model_name),
               K_(func_type));
private:
  common::ObIAllocator *allocator_;
  uint64_t table_id_;
  uint64_t column_id_;
  uint64_t state_column_id_;
  uint64_t request_column_id_;
  uint64_t result_spec_column_id_;
  int64_t max_retries_;
  ObAiColumnSyncMode sync_mode_;
  int64_t definition_version_;
  common::ObString column_name_;
  common::ObString normalized_expr_;
  common::ObString model_name_;
  common::ObString func_type_;
};

/**
 * A pending generation request for one row.
 *
 * One request covers every AI column of the row that was invalidated by the same
 * data change: the columns of one request are published together so that a reader
 * never observes a half-updated row.
 */
class ObAiColumnTask
{
public:
  ObAiColumnTask()
      : table_id_(OB_INVALID_ID),
        column_id_(OB_INVALID_ID),
        request_id_(0),
        definition_version_(0),
        state_(ObAiColumnTaskState::PENDING),
        retry_count_(0),
        eligible_ts_(0),
        last_error_()
  {}

  void reset();

  uint64_t get_table_id() const { return table_id_; }
  uint64_t get_column_id() const { return column_id_; }
  uint64_t get_request_id() const { return request_id_; }
  int64_t get_definition_version() const { return definition_version_; }
  ObAiColumnTaskState get_state() const { return state_; }
  int64_t get_retry_count() const { return retry_count_; }
  int64_t get_eligible_ts() const { return eligible_ts_; }
  const common::ObString &get_rowkey_predicate() const { return rowkey_predicate_; }
  const common::ObString &get_last_error() const { return last_error_; }

  void set_table_id(uint64_t id) { table_id_ = id; }
  void set_column_id(uint64_t id) { column_id_ = id; }
  void set_request_id(uint64_t id) { request_id_ = id; }
  void set_definition_version(int64_t v) { definition_version_ = v; }
  void set_state(ObAiColumnTaskState state) { state_ = state; }
  void set_retry_count(int64_t v) { retry_count_ = v; }
  void set_eligible_ts(int64_t v) { eligible_ts_ = v; }
  int set_rowkey_predicate(common::ObIAllocator &allocator, const common::ObString &predicate);
  int set_last_error(common::ObIAllocator &allocator, const common::ObString &err);

  TO_STRING_KV(K_(table_id),
               K_(column_id),
               K_(request_id),
               K_(definition_version),
               K_(state),
               K_(retry_count),
               K_(eligible_ts),
               K_(rowkey_predicate),
               K_(last_error));
private:
  uint64_t table_id_;
  uint64_t column_id_;
  uint64_t request_id_;
  int64_t definition_version_;
  ObAiColumnTaskState state_;
  int64_t retry_count_;
  int64_t eligible_ts_;
  common::ObString rowkey_predicate_;
  common::ObString last_error_;
};

} // namespace share
} // namespace oceanbase
