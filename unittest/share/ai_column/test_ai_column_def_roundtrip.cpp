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

// Round-trip regression for the AI column definition wire format.
//
// The definition travels inside the table schema, so an encoding that does not
// consume exactly the bytes it produced would desynchronise the whole schema
// stream. The empty-string case is the interesting one: an empty field followed
// by a non-empty field must not shift the following fields.

#include "share/ai_service/ob_ai_column_def.h"
#include "lib/allocator/page_arena.h"
#include "lib/utility/serialization.h"

#include <cassert>
#include <cstdio>
#include <string>

using namespace oceanbase::common;
using namespace oceanbase::share;

namespace
{

int check_round_trip(ObIAllocator &allocator, const ObAiColumnDef &src, const char *label)
{
  int ret = OB_SUCCESS;
  ObAiColumnDef dst;
  dst.set_allocator(&allocator);

  const int64_t buf_len = src.get_serialize_size();
  char *buf = static_cast<char *>(allocator.alloc(buf_len + 64));
  if (OB_ISNULL(buf)) {
    fprintf(stderr, "%s: failed to allocate buffer\n", label);
    return OB_ALLOCATE_MEMORY_FAILED;
  }
  int64_t pos = 0;
  if (OB_FAIL(src.serialize(buf, buf_len, pos))) {
    fprintf(stderr, "%s: serialize failed, ret=%d\n", label, ret);
    return ret;
  }
  if (pos > buf_len) {
    fprintf(stderr, "%s: serialize wrote %ld bytes into a %ld byte buffer\n", label, pos, buf_len);
    return OB_ERR_UNEXPECTED;
  }

  int64_t read_pos = 0;
  if (OB_FAIL(dst.deserialize(buf, pos, read_pos))) {
    fprintf(stderr, "%s: deserialize failed, ret=%d\n", label, ret);
    return ret;
  }
  if (read_pos != pos) {
    fprintf(stderr, "%s: consumed %ld bytes but %ld were written\n", label, read_pos, pos);
    return OB_ERR_UNEXPECTED;
  }

  if (dst.get_table_id() != src.get_table_id()
      || dst.get_column_id() != src.get_column_id()
      || dst.get_state_column_id() != src.get_state_column_id()
      || dst.get_request_column_id() != src.get_request_column_id()
      || dst.get_result_spec_column_id() != src.get_result_spec_column_id()
      || dst.get_max_retries() != src.get_max_retries()
      || dst.get_sync_mode() != src.get_sync_mode()
      || dst.get_definition_version() != src.get_definition_version()) {
    fprintf(stderr, "%s: numeric fields differ after the round trip\n", label);
    return OB_ERR_UNEXPECTED;
  }
  if (dst.get_column_name() != src.get_column_name()
      || dst.get_normalized_expr() != src.get_normalized_expr()
      || dst.get_model_name() != src.get_model_name()
      || dst.get_func_type() != src.get_func_type()) {
    fprintf(stderr, "%s: string fields differ after the round trip\n", label);
    return OB_ERR_UNEXPECTED;
  }
  return OB_SUCCESS;
}

int test_round_trip()
{
  int ret = OB_SUCCESS;
  ObArenaAllocator allocator;

  // A fully populated definition.
  {
    ObAiColumnDef def(&allocator);
    def.set_table_id(1001);
    def.set_column_id(7);
    def.set_state_column_id(8);
    def.set_request_column_id(9);
    def.set_result_spec_column_id(10);
    def.set_max_retries(2);
    def.set_sync_mode(ObAiColumnSyncMode::ASYNC);
    def.set_definition_version(3);
    if (OB_FAIL(def.set_column_name(allocator, ObString("tags")))) { return ret; }
    if (OB_FAIL(def.set_normalized_expr(allocator, ObString("AI_COMPLETE('m', content)")))) { return ret; }
    if (OB_FAIL(def.set_model_name(allocator, ObString("m")))) { return ret; }
    if (OB_FAIL(def.set_func_type(allocator, ObString("COMPLETION")))) { return ret; }
    if (OB_FAIL(check_round_trip(allocator, def, "populated"))) { return ret; }
  }

  // An empty first string followed by non-empty ones: this is the case that a
  // double length prefix would break.
  {
    ObAiColumnDef def(&allocator);
    def.set_table_id(1002);
    def.set_column_id(11);
    def.set_max_retries(0);
    def.set_definition_version(1);
    if (OB_FAIL(def.set_column_name(allocator, ObString("")))) { return ret; }
    if (OB_FAIL(def.set_normalized_expr(allocator, ObString("expr")))) { return ret; }
    if (OB_FAIL(def.set_model_name(allocator, ObString("model")))) { return ret; }
    if (OB_FAIL(def.set_func_type(allocator, ObString("")))) { return ret; }
    if (OB_FAIL(check_round_trip(allocator, def, "empty-first"))) { return ret; }
  }

  // All strings empty.
  {
    ObAiColumnDef def(&allocator);
    def.set_table_id(1003);
    def.set_column_id(12);
    if (OB_FAIL(check_round_trip(allocator, def, "all-empty"))) { return ret; }
  }

  // A definition with an empty middle field.
  {
    ObAiColumnDef def(&allocator);
    def.set_table_id(1004);
    def.set_column_id(13);
    if (OB_FAIL(def.set_column_name(allocator, ObString("name")))) { return ret; }
    if (OB_FAIL(def.set_normalized_expr(allocator, ObString("")))) { return ret; }
    if (OB_FAIL(def.set_model_name(allocator, ObString("")))) { return ret; }
    if (OB_FAIL(def.set_func_type(allocator, ObString("EMBED")))) { return ret; }
    if (OB_FAIL(check_round_trip(allocator, def, "empty-middle"))) { return ret; }
  }

  // A long string exercises the multi-byte length prefix of encode_vstr.
  {
    ObAiColumnDef def(&allocator);
    std::string long_expr(5000, 'x');
    def.set_table_id(1005);
    def.set_column_id(14);
    if (OB_FAIL(def.set_column_name(allocator, ObString("long")))) { return ret; }
    if (OB_FAIL(def.set_normalized_expr(allocator, ObString(long_expr.c_str())))) { return ret; }
    if (OB_FAIL(def.set_model_name(allocator, ObString("m")))) { return ret; }
    if (OB_FAIL(def.set_func_type(allocator, ObString("COMPLETION")))) { return ret; }
    if (OB_FAIL(check_round_trip(allocator, def, "long-expr"))) { return ret; }
  }
  return ret;
}

/**
 * Two definitions serialized one after another must decode independently: this is
 * what happens when a table has more than one AI column.
 */
int test_two_definitions_in_one_stream()
{
  int ret = OB_SUCCESS;
  ObArenaAllocator allocator;

  ObAiColumnDef first(&allocator);
  first.set_table_id(2001);
  first.set_column_id(1);
  if (OB_FAIL(first.set_column_name(allocator, ObString("")))) { return ret; }
  if (OB_FAIL(first.set_normalized_expr(allocator, ObString("e1")))) { return ret; }
  if (OB_FAIL(first.set_model_name(allocator, ObString("m1")))) { return ret; }
  if (OB_FAIL(first.set_func_type(allocator, ObString("COMPLETION")))) { return ret; }

  ObAiColumnDef second(&allocator);
  second.set_table_id(2001);
  second.set_column_id(2);
  if (OB_FAIL(second.set_column_name(allocator, ObString("c2")))) { return ret; }
  if (OB_FAIL(second.set_normalized_expr(allocator, ObString("")))) { return ret; }
  if (OB_FAIL(second.set_model_name(allocator, ObString("m2")))) { return ret; }
  if (OB_FAIL(second.set_func_type(allocator, ObString("")))) { return ret; }

  const int64_t buf_len = first.get_serialize_size() + second.get_serialize_size();
  char *buf = static_cast<char *>(allocator.alloc(buf_len + 64));
  if (OB_ISNULL(buf)) { return OB_ALLOCATE_MEMORY_FAILED; }
  int64_t pos = 0;
  if (OB_FAIL(first.serialize(buf, buf_len, pos))) { return ret; }
  const int64_t first_end = pos;
  if (OB_FAIL(second.serialize(buf, buf_len, pos))) { return ret; }

  ObAiColumnDef d1;
  d1.set_allocator(&allocator);
  ObAiColumnDef d2;
  d2.set_allocator(&allocator);
  int64_t read_pos = 0;
  if (OB_FAIL(d1.deserialize(buf, first_end, read_pos))) { return ret; }
  if (read_pos != first_end) {
    fprintf(stderr, "two-definitions: first definition consumed %ld of %ld bytes\n",
            read_pos, first_end);
    return OB_ERR_UNEXPECTED;
  }
  if (OB_FAIL(d2.deserialize(buf, pos, read_pos))) { return ret; }
  if (read_pos != pos) {
    fprintf(stderr, "two-definitions: second definition consumed %ld of %ld bytes\n",
            read_pos, pos);
    return OB_ERR_UNEXPECTED;
  }
  if (d1.get_column_name() != ObString("") || d1.get_normalized_expr() != ObString("e1")) {
    fprintf(stderr, "two-definitions: first definition decoded wrongly\n");
    return OB_ERR_UNEXPECTED;
  }
  if (d2.get_column_name() != ObString("c2") || d2.get_normalized_expr() != ObString("")) {
    fprintf(stderr, "two-definitions: second definition decoded wrongly\n");
    return OB_ERR_UNEXPECTED;
  }
  return ret;
}

} // namespace

int main()
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ret = test_round_trip())) {
    fprintf(stderr, "ai column def round trip FAILED, ret=%d\n", ret);
    return 1;
  }
  if (OB_FAIL(ret = test_two_definitions_in_one_stream())) {
    fprintf(stderr, "ai column def multi-definition stream FAILED, ret=%d\n", ret);
    return 1;
  }
  printf("ai_column_def_round_trip=ok\n");
  return 0;
}
