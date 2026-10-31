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

#include "share/ai_service/ob_ai_column_def.h"
#include "lib/utility/serialization.h"

namespace oceanbase
{
namespace share
{

namespace
{
/**
 * Copy a string into memory owned by the given allocator.
 *
 * The AI column definition lives as long as the schema object that holds it, so the
 * copy must not be owned by a transient arena.
 */
int ai_column_deep_copy_str(common::ObIAllocator &allocator,
                             const common::ObString &src,
                             common::ObString &dest)
{
  int ret = OB_SUCCESS;
  if (src.length() == 0) {
    dest.reset();
  } else {
    char *buf = static_cast<char *>(allocator.alloc(src.length() + 1));
    if (OB_ISNULL(buf)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_ERROR("fail to allocate memory for ai column string", K(ret), K(src.length()));
    } else {
      memcpy(buf, src.ptr(), src.length());
      buf[src.length()] = '\0';
      dest.assign_ptr(buf, src.length());
    }
  }
  return ret;
}

/**
 * Encode a string as a length-prefixed blob.
 *
 * `encode_vstr` already stores the length, so no second length prefix is written.
 * The decoder therefore always consumes exactly the bytes the encoder produced,
 * including for an empty string.
 */
int ai_column_encode_str(char *buf, const int64_t buf_len, int64_t &pos, const common::ObString &str)
{
  return serialization::encode_vstr(buf, buf_len, pos, str.ptr(), str.length());
}

/**
 * Decode a string written by ai_column_encode_str.
 *
 * The payload is copied into the schema allocator so that the decoded definition
 * does not keep a pointer into the wire buffer.
 */
int ai_column_decode_str(const char *buf, const int64_t data_len, int64_t &pos,
                          common::ObIAllocator &allocator, common::ObString &str)
{
  int ret = OB_SUCCESS;
  int64_t len = 0;
  const char *payload = NULL;
  if (OB_ISNULL(payload = serialization::decode_vstr(buf, data_len, pos, &len))) {
    ret = OB_ERR_UNEXPECTED;
  } else if (len < 0) {
    ret = OB_ERR_UNEXPECTED;
  } else if (0 == len) {
    str.reset();
  } else if (OB_FAIL(ai_column_deep_copy_str(allocator,
                                             common::ObString(static_cast<int32_t>(len), payload),
                                             str))) {
  }
  return ret;
}

/**
 * Number of bytes ai_column_encode_str writes for the given string.
 *
 * `encode_vstr` writes a variable length integer holding the payload length,
 * then the payload itself and then the terminating NUL, so the encoded length
 * is exactly `encoded_length_vstr`.
 */
int64_t ai_column_encoded_str_size(const common::ObString &str)
{
  return serialization::encoded_length_vstr(str.length());
}
} // namespace

const char *ob_ai_column_state_to_string(ObAiColumnState state)
{
  switch (state) {
  case ObAiColumnState::PENDING: return "PENDING";
  case ObAiColumnState::READY:   return "READY";
  case ObAiColumnState::FAILED:  return "FAILED";
  default:                       return "UNKNOWN";
  }
}

const char *ob_ai_column_task_state_to_string(ObAiColumnTaskState state)
{
  switch (state) {
  case ObAiColumnTaskState::PENDING:  return "PENDING";
  case ObAiColumnTaskState::RUNNING:  return "RUNNING";
  case ObAiColumnTaskState::RETRYING: return "RETRYING";
  case ObAiColumnTaskState::READY:    return "READY";
  case ObAiColumnTaskState::FAILED:   return "FAILED";
  default:                            return "UNKNOWN";
  }
}

void ObAiColumnDef::reset()
{
  table_id_ = OB_INVALID_ID;
  column_id_ = OB_INVALID_ID;
  state_column_id_ = OB_INVALID_ID;
  request_column_id_ = OB_INVALID_ID;
  result_spec_column_id_ = OB_INVALID_ID;
  max_retries_ = 0;
  sync_mode_ = ObAiColumnSyncMode::ASYNC;
  definition_version_ = 0;
  column_name_.reset();
  normalized_expr_.reset();
  model_name_.reset();
  func_type_.reset();
}

int ObAiColumnDef::assign(common::ObIAllocator &allocator, const ObAiColumnDef &other)
{
  int ret = OB_SUCCESS;
  reset();
  allocator_ = &allocator;
  if (OB_FAIL(set_column_name(allocator, other.column_name_))) {
  } else if (OB_FAIL(set_normalized_expr(allocator, other.normalized_expr_))) {
  } else if (OB_FAIL(set_model_name(allocator, other.model_name_))) {
  } else if (OB_FAIL(set_func_type(allocator, other.func_type_))) {
  } else {
    table_id_ = other.table_id_;
    column_id_ = other.column_id_;
    state_column_id_ = other.state_column_id_;
    request_column_id_ = other.request_column_id_;
    result_spec_column_id_ = other.result_spec_column_id_;
    max_retries_ = other.max_retries_;
    sync_mode_ = other.sync_mode_;
    definition_version_ = other.definition_version_;
  }
  return ret;
}

int ObAiColumnDef::set_column_name(common::ObIAllocator &allocator, const common::ObString &name)
{
  return ai_column_deep_copy_str(allocator, name, column_name_);
}

int ObAiColumnDef::set_normalized_expr(common::ObIAllocator &allocator, const common::ObString &expr)
{
  return ai_column_deep_copy_str(allocator, expr, normalized_expr_);
}

int ObAiColumnDef::set_model_name(common::ObIAllocator &allocator, const common::ObString &name)
{
  return ai_column_deep_copy_str(allocator, name, model_name_);
}

int ObAiColumnDef::set_func_type(common::ObIAllocator &allocator, const common::ObString &type)
{
  return ai_column_deep_copy_str(allocator, type, func_type_);
}

int64_t ObAiColumnDef::get_serialize_size() const
{
  int64_t size = 0;
  // The 8 fixed-width fields are written by encode_vi64, so each one takes
  // exactly as many bytes as its own value needs.
  size += serialization::encoded_length_vi64(static_cast<int64_t>(table_id_));
  size += serialization::encoded_length_vi64(static_cast<int64_t>(column_id_));
  size += serialization::encoded_length_vi64(static_cast<int64_t>(state_column_id_));
  size += serialization::encoded_length_vi64(static_cast<int64_t>(request_column_id_));
  size += serialization::encoded_length_vi64(static_cast<int64_t>(result_spec_column_id_));
  size += serialization::encoded_length_vi64(max_retries_);
  size += serialization::encoded_length_vi64(static_cast<int64_t>(sync_mode_));
  size += serialization::encoded_length_vi64(definition_version_);
  // The 4 strings are written by encode_vstr, which prefixes the payload with a
  // variable length integer holding its length.
  size += ai_column_encoded_str_size(column_name_);
  size += ai_column_encoded_str_size(normalized_expr_);
  size += ai_column_encoded_str_size(model_name_);
  size += ai_column_encoded_str_size(func_type_);
  return size;
}

int ObAiColumnDef::serialize(char *buf, const int64_t buf_len, int64_t &pos) const
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(serialization::encode_vi64(buf, buf_len, pos, static_cast<int64_t>(table_id_)))) {
  } else if (OB_FAIL(serialization::encode_vi64(buf, buf_len, pos, static_cast<int64_t>(column_id_)))) {
  } else if (OB_FAIL(serialization::encode_vi64(buf, buf_len, pos, static_cast<int64_t>(state_column_id_)))) {
  } else if (OB_FAIL(serialization::encode_vi64(buf, buf_len, pos, static_cast<int64_t>(request_column_id_)))) {
  } else if (OB_FAIL(serialization::encode_vi64(buf, buf_len, pos, static_cast<int64_t>(result_spec_column_id_)))) {
  } else if (OB_FAIL(serialization::encode_vi64(buf, buf_len, pos, max_retries_))) {
  } else if (OB_FAIL(serialization::encode_vi64(buf, buf_len, pos, static_cast<int64_t>(sync_mode_)))) {
  } else if (OB_FAIL(serialization::encode_vi64(buf, buf_len, pos, definition_version_))) {
  } else if (OB_FAIL(ai_column_encode_str(buf, buf_len, pos, column_name_))) {
  } else if (OB_FAIL(ai_column_encode_str(buf, buf_len, pos, normalized_expr_))) {
  } else if (OB_FAIL(ai_column_encode_str(buf, buf_len, pos, model_name_))) {
  } else if (OB_FAIL(ai_column_encode_str(buf, buf_len, pos, func_type_))) {
  }
  return ret;
}

int ObAiColumnDef::deserialize(const char *buf, const int64_t data_len, int64_t &pos)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(allocator_)) {
    // The definition owns the strings it decodes, so it must know the allocator
    // they have to be copied into.
    ret = OB_ERR_UNEXPECTED;
    LOG_ERROR("ai column def has no allocator", K(ret));
  }
  int64_t table_id = OB_INVALID_ID;
  int64_t column_id = OB_INVALID_ID;
  int64_t state_column_id = OB_INVALID_ID;
  int64_t request_column_id = OB_INVALID_ID;
  int64_t result_spec_column_id = OB_INVALID_ID;
  int64_t max_retries = 0;
  int64_t sync_mode = 0;
  int64_t definition_version = 0;
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(serialization::decode_vi64(buf, data_len, pos, &table_id))) {
  } else if (OB_FAIL(serialization::decode_vi64(buf, data_len, pos, &column_id))) {
  } else if (OB_FAIL(serialization::decode_vi64(buf, data_len, pos, &state_column_id))) {
  } else if (OB_FAIL(serialization::decode_vi64(buf, data_len, pos, &request_column_id))) {
  } else if (OB_FAIL(serialization::decode_vi64(buf, data_len, pos, &result_spec_column_id))) {
  } else if (OB_FAIL(serialization::decode_vi64(buf, data_len, pos, &max_retries))) {
  } else if (OB_FAIL(serialization::decode_vi64(buf, data_len, pos, &sync_mode))) {
  } else if (OB_FAIL(serialization::decode_vi64(buf, data_len, pos, &definition_version))) {
  } else if (OB_FAIL(ai_column_decode_str(buf, data_len, pos, *allocator_, column_name_))) {
  } else if (OB_FAIL(ai_column_decode_str(buf, data_len, pos, *allocator_, normalized_expr_))) {
  } else if (OB_FAIL(ai_column_decode_str(buf, data_len, pos, *allocator_, model_name_))) {
  } else if (OB_FAIL(ai_column_decode_str(buf, data_len, pos, *allocator_, func_type_))) {
  } else {
    table_id_ = static_cast<uint64_t>(table_id);
    column_id_ = static_cast<uint64_t>(column_id);
    state_column_id_ = static_cast<uint64_t>(state_column_id);
    request_column_id_ = static_cast<uint64_t>(request_column_id);
    result_spec_column_id_ = static_cast<uint64_t>(result_spec_column_id);
    max_retries_ = max_retries;
    sync_mode_ = static_cast<ObAiColumnSyncMode>(sync_mode);
    definition_version_ = definition_version;
  }
  return ret;
}

void ObAiColumnTask::reset()
{
  table_id_ = OB_INVALID_ID;
  column_id_ = OB_INVALID_ID;
  request_id_ = 0;
  definition_version_ = 0;
  state_ = ObAiColumnTaskState::PENDING;
  retry_count_ = 0;
  eligible_ts_ = 0;
  rowkey_predicate_.reset();
  last_error_.reset();
}

int ObAiColumnTask::set_rowkey_predicate(common::ObIAllocator &allocator, const common::ObString &predicate)
{
  return ai_column_deep_copy_str(allocator, predicate, rowkey_predicate_);
}

int ObAiColumnTask::set_last_error(common::ObIAllocator &allocator, const common::ObString &err)
{
  return ai_column_deep_copy_str(allocator, err, last_error_);
}

} // namespace share
} // namespace oceanbase
