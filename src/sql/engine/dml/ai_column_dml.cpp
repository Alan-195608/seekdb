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

#define USING_LOG_PREFIX SQL_ENG
#include "sql/engine/dml/ai_column_dml.h"
#include "sql/engine/dml/ob_table_modify_op.h"
#include "sql/engine/ob_exec_context.h"
#include "sql/ob_sql_context.h"
#include "sql/parser/ob_parser.h"
#include "share/schema/ob_schema_getter_guard.h"
#include "share/schema/ob_table_schema.h"
#include "lib/allocator/page_arena.h"
#include "lib/string/ob_sql_string.h"
#include "lib/time/ob_time_utility.h"
#include "common/mysqlclient/ob_mysql_proxy.h"
#include "query/session/ob_inner_sql_connection_access.h"
#include "sql/session/ob_inner_sql_connection.h"
namespace oceanbase { namespace sql {
using namespace common;
using namespace common::sqlclient;
using namespace share;
using namespace share::schema;
namespace {
thread_local int internal_depth = 0;
thread_local uint64_t retry_column = OB_INVALID_ID;
thread_local ObString retry_predicate;
int quote_name(ObSqlString &sql, const ObString &name)
{
  int ret = sql.append("`");
  for (int64_t i=0; OB_SUCC(ret) && i<name.length(); ++i) {
    if (name.ptr()[i]=='`') { ret=sql.append("``"); }
    else { ret=sql.append(name.ptr()+i,1); }
  }
  if (OB_SUCC(ret)) { ret=sql.append("`"); }
  return ret;
}
int hex_literal(ObSqlString &sql, const ObString &value)
{
  int ret = sql.append("X'");
  static const char digits[] = "0123456789ABCDEF";
  for (int64_t i = 0; OB_SUCC(ret) && i < value.length(); ++i) {
    const unsigned char ch = static_cast<unsigned char>(value.ptr()[i]);
    const char pair[] = {digits[ch >> 4], digits[ch & 15]};
    ret = sql.append(pair, 2);
  }
  if (OB_SUCC(ret)) { ret = sql.append("'"); }
  return ret;
}
int refs(ParseNode *node, ObIArray<ObString> &names)
{
  int ret=OB_SUCCESS;
  if (node != nullptr) {
    if (node->type_==T_COLUMN_REF && node->num_child_==3 && node->children_[2]!=nullptr) {
      auto *n=node->children_[2];
      ObString value(n->str_len_, n->str_value_);
      bool seen=false;
      for (int64_t i=0;i<names.count();++i) { seen |= names.at(i).case_compare(value)==0; }
      if (!seen) { ret=names.push_back(value); }
    } else {
      for (int64_t i=0;OB_SUCC(ret) && i<node->num_child_;++i) { ret=refs(node->children_[i],names); }
    }
  }
  return ret;
}
struct Change { ObString key; ObSEArray<ObString,4> fingerprints; ObSEArray<bool,4> changed; };
int copy_string(ObIAllocator &arena,const ObString &in,ObString &out)
{ return ob_write_string(arena,in,out); }
}
bool ai_column_internal_sql_active() { return internal_depth>0; }
bool ai_column_retry_active() { return retry_column != OB_INVALID_ID; }
AiColumnInternalSQLGuard::AiColumnInternalSQLGuard() { ++internal_depth; }
AiColumnInternalSQLGuard::~AiColumnInternalSQLGuard() { --internal_depth; }
AiColumnRetryGuard::AiColumnRetryGuard(uint64_t column_id, const ObString &predicate)
  : previous_(retry_column), previous_predicate_(retry_predicate)
{ retry_column = column_id; retry_predicate = predicate; }
AiColumnRetryGuard::~AiColumnRetryGuard()
{ retry_column = previous_; retry_predicate = previous_predicate_; }
int check_ai_column_strict_read(ObExecContext &ctx, uint64_t table_id,
                                const ObIArray<uint64_t> &column_ids)
{
  int ret = OB_SUCCESS;
  if (ai_column_internal_sql_active() || retry_column != OB_INVALID_ID) { return ret; }
  auto *session = ctx.get_my_session();
  auto *sql_ctx = ctx.get_sql_ctx();
  bool enabled = false;
  const ObTableSchema *table = nullptr;
  const ObDatabaseSchema *database = nullptr;
  if (session == nullptr || sql_ctx == nullptr || sql_ctx->schema_guard_ == nullptr) {
    return OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(session->get_sys_variable(SYS_VAR_AI_COLUMN_STRICT_READ, enabled))) {
    return ret;
  } else if (!enabled) { return ret; }
  auto &guard = *sql_ctx->schema_guard_;
  if (OB_FAIL(guard.get_table_schema(table_id, table))) { return ret; }
  if (table == nullptr || !table->has_ai_column()) { return ret; }
  if (OB_FAIL(guard.get_database_schema(table->get_database_id(), database))) { return ret; }
  if (database == nullptr) { return OB_ERR_UNEXPECTED; }
  ObSqlString qualified, predicate, strict_sql;
  if (OB_FAIL(quote_name(qualified, database->get_database_name_str()))) {
  } else if (OB_FAIL(qualified.append("."))) {
  } else if (OB_FAIL(quote_name(qualified, table->get_table_name_str()))) {
  }
  // UPDATE/DELETE fetch old row fields for index maintenance; these implicit
  // access columns must not turn an ordinary-column statement into an AI read.
  ObArenaAllocator parse_arena;
  ObParser statement_parser(parse_arena, session->get_sql_mode());
  ParseResult statement_tree;
  ObSEArray<ObString, 16> explicit_names;
  bool filter_dml_references = false;
  const ObString current_sql = sql_ctx->cur_sql_.empty()
      ? session->get_current_query_string() : sql_ctx->cur_sql_;
  if (!current_sql.empty() && OB_SUCCESS == statement_parser.parse(current_sql, statement_tree)) {
    ParseNode *statement = statement_tree.result_tree_;
    if (statement != nullptr && statement->type_ == T_STMT_LIST && statement->num_child_ > 0) {
      statement = statement->children_[0];
    }
    if (statement != nullptr && (statement->type_ == T_UPDATE || statement->type_ == T_DELETE)) {
      if (OB_FAIL(refs(statement, explicit_names))) { return ret; }
      filter_dml_references = true;
    }
  }
  uint64_t checked_column = OB_INVALID_ID;
  for (int64_t i = 0; OB_SUCC(ret) && i < table->get_ai_column_defs().count(); ++i) {
    const auto &def = table->get_ai_column_defs().at(i);
    bool referenced = false;
    for (int64_t j = 0; j < column_ids.count(); ++j) { referenced |= column_ids.at(j) == def.get_column_id(); }
    if (filter_dml_references) {
      referenced = false;
      for (int64_t j = 0; j < explicit_names.count(); ++j) {
        referenced |= explicit_names.at(j).case_compare(def.get_column_name()) == 0;
      }
    }
    if (!referenced) { continue; }
    const auto *state = table->get_column_schema(def.get_state_column_id());
    if (state == nullptr) { ret = OB_ERR_UNEXPECTED; }
    else {
      if (predicate.length() > 0) { ret = predicate.append(" OR "); }
      if (OB_SUCC(ret)) { ret = predicate.append("COALESCE("); }
      if (OB_SUCC(ret)) { ret = quote_name(predicate, state->get_column_name_str()); }
      if (OB_SUCC(ret)) { ret = predicate.append(",'PENDING') <> 'READY'"); }
      checked_column = def.get_column_id();
    }
  }
  if (OB_FAIL(ret) || predicate.length() == 0) { return ret; }
  if (OB_FAIL(strict_sql.append_fmt("SELECT 1 FROM %s WHERE %s LIMIT 1", qualified.ptr(), predicate.ptr()))) { return ret; }
  AiColumnInternalSQLGuard internal;
  ObISQLConnectionGuard connection_guard;
  bool created = session->get_inner_conn() == nullptr;
  if (created) {
    if (OB_FAIL(query::ObInnerSQLConnectionAccess::create_connection_with_external_session(session, connection_guard))) { return ret; }
    session->set_inner_conn(connection_guard.get_ptr());
  }
  auto *connection = as_inner_sql_connection(session->get_inner_conn());
  ObSQLSessionInfo::StmtSavedValue saved_session;
  ObIInnerSQLConnection::SavedValue saved_connection;
  if (connection == nullptr) { ret = OB_ERR_UNEXPECTED; }
  else if (OB_FAIL(connection->begin_nested_session(saved_session, saved_connection, true))) {
  } else {
    {
      SMART_VAR(ObMySQLProxy::MySQLResult, result) {
        if (OB_FAIL(connection->execute_read(strict_sql.ptr(), result))) {
        } else if (result.get_result() == nullptr) { ret = OB_ERR_UNEXPECTED; }
        else {
          ret = result.get_result()->next();
          if (ret == OB_ITER_END) { ret = OB_SUCCESS; }
          else if (OB_SUCC(ret)) {
            ret = OB_AI_COLUMN_NOT_READY;
            LOG_USER_ERROR(OB_AI_COLUMN_NOT_READY, table_id, checked_column);
          }
        }
      }
    }
    const int end_ret = connection->end_nested_session(saved_session, saved_connection);
    if (OB_SUCC(ret)) { ret = end_ret; }
  }
  if (created) { session->set_inner_conn(nullptr); }
  return ret;
}
int synchronize_ai_columns(ObTableModifyOp &op, ObExecContext &ctx, uint64_t table_id)
{
  int ret=OB_SUCCESS;
  if (ai_column_internal_sql_active()) { return ret; }
  auto *sql_ctx=ctx.get_sql_ctx();
  auto *session=ctx.get_my_session();
  const ObTableSchema *table=nullptr;
  const ObDatabaseSchema *database=nullptr;
  if (sql_ctx==nullptr || sql_ctx->schema_guard_==nullptr || session==nullptr) { return OB_ERR_UNEXPECTED; }
  auto &guard=*sql_ctx->schema_guard_;
  if (OB_FAIL(guard.get_table_schema(table_id,table))) {
  } else if (table==nullptr || !table->has_ai_column()) { return ret;
  } else if (OB_FAIL(guard.get_database_schema(table->get_database_id(),database))) {
  } else if (database==nullptr) { ret=OB_ERR_UNEXPECTED; }
  if (OB_FAIL(ret)) { return ret; }
  const auto &defs=table->get_ai_column_defs();
  ObArenaAllocator arena;
  ObSqlString qualified,key_expr,select_sql;
  if (OB_FAIL(quote_name(qualified,database->get_database_name_str()))) {
  } else if (OB_FAIL(qualified.append("."))) {
  } else if (OB_FAIL(quote_name(qualified,table->get_table_name_str()))) {
  } else if (OB_FAIL(key_expr.append("CONCAT_WS(' AND '"))) {
  }
  ObSEArray<ObString,8> key_names;
  const auto &rowkeys=table->get_rowkey_info();
  for (int64_t i=0;OB_SUCC(ret) && i<rowkeys.get_size();++i) {
    uint64_t id=0;
    if (OB_FAIL(rowkeys.get_column_id(i,id))) { break; }
    const auto *col=table->get_column_schema(id);
    if (col==nullptr) { ret=OB_ERR_UNEXPECTED; break; }
    ObSqlString name; ObSqlString label;
    if (OB_FAIL(quote_name(name,col->get_column_name_str()))) {
    } else if (OB_FAIL(key_names.push_back(col->get_column_name_str()))) {
    } else if (OB_FAIL(label.append_fmt("%s = ",name.ptr()))) {
    } else if (OB_FAIL(key_expr.append(",CONCAT("))) {
    } else if (OB_FAIL(hex_literal(key_expr,label.string()))) {
    } else if (OB_FAIL(key_expr.append(","))) {
    } else if (ob_is_numeric_type(col->get_data_type())) {
      ret=key_expr.append_fmt("CAST(%s AS CHAR))",name.ptr());
    } else { ret=key_expr.append_fmt("CONCAT('X',CHAR(39),HEX(%s),CHAR(39)))",name.ptr()); }
  }
  if (OB_SUCC(ret)) { ret=key_expr.append(")"); }
  if (OB_SUCC(ret)) { ret=select_sql.append_fmt("SELECT %s",key_expr.ptr()); }
  for (int64_t i=0;OB_SUCC(ret) && i<defs.count();++i) {
    ObParser parser(arena,session->get_sql_mode()); ParseResult parsed;
    ObSqlString expression; ObSEArray<ObString,8> deps;
    if (OB_FAIL(expression.append_fmt("SELECT %.*s",defs.at(i).get_normalized_expr().length(),defs.at(i).get_normalized_expr().ptr()))) {
    } else if (OB_FAIL(parser.parse(expression.string(),parsed))) {
    } else if (OB_FAIL(refs(parsed.result_tree_,deps))) {
    } else if (OB_FAIL(select_sql.append(",SHA2(JSON_ARRAY("))) {
    }
    for (int64_t j=0;OB_SUCC(ret) && j<key_names.count();++j) {
      if (j>0) { ret=select_sql.append(","); }
      if (OB_SUCC(ret)) { ret=quote_name(select_sql,key_names.at(j)); }
    }
    for (int64_t j=0;OB_SUCC(ret) && j<deps.count();++j) {
      if (key_names.count()>0 || j>0) { ret=select_sql.append(","); }
      if (OB_SUCC(ret)) { ret=quote_name(select_sql,deps.at(j)); }
    }
    const auto *spec=table->get_column_schema(defs.at(i).get_result_spec_column_id());
    if (spec==nullptr) { ret=OB_ERR_UNEXPECTED; }
    if (OB_SUCC(ret)) { ret=select_sql.append("),256),COALESCE("); }
    if (OB_SUCC(ret)) { ret=quote_name(select_sql,spec->get_column_name_str()); }
    if (OB_SUCC(ret)) { ret=select_sql.append(",'')"); }
  }
  if (OB_SUCC(ret)) { ret=select_sql.append_fmt(" FROM %s",qualified.ptr()); }
  if (OB_SUCC(ret) && retry_column != OB_INVALID_ID && !retry_predicate.empty()) {
    ret = select_sql.append_fmt(" WHERE %.*s", retry_predicate.length(), retry_predicate.ptr());
  }
  if (OB_FAIL(ret)) { return ret; }
  AiColumnInternalSQLGuard internal;
  struct ReadOwnStatementGuard {
    ObExecContext &ctx;
    bool previous;
    explicit ReadOwnStatementGuard(ObExecContext &value)
        : ctx(value), previous(value.get_das_ctx().iter_uncommitted_row_)
    { ctx.get_das_ctx().iter_uncommitted_row_ = true; }
    ~ReadOwnStatementGuard() { ctx.get_das_ctx().iter_uncommitted_row_ = previous; }
  } read_own_statement(ctx);
  if (OB_FAIL(op.open_inner_conn())) { return ret; }
  if (OB_FAIL(op.begin_nested_session(true))) { return ret; }
  ObSEArray<Change,16> changes;
  {
    SMART_VAR(ObMySQLProxy::MySQLResult,res) {
      if (OB_FAIL(op.execute_read(select_sql.ptr(),res))) {
      } else if (res.get_result()==nullptr) { ret=OB_ERR_UNEXPECTED;
      } else {
        auto &rows=*res.get_result();
        while (OB_SUCC(ret) && OB_SUCC(ret = rows.next())) {
          Change change; ObString key;
          if (OB_FAIL(rows.get_varchar(int64_t(0),key))) { break; }
          if (OB_FAIL(copy_string(arena,key,change.key))) { break; }
          bool any=false;
          for (int64_t i=0;OB_SUCC(ret) && i<defs.count();++i) {
            ObString now,old,copied;
            if (OB_FAIL(rows.get_varchar(1+2*i,now))) {
            } else if (OB_FAIL(rows.get_varchar(2+2*i,old))) {
            } else if (OB_FAIL(copy_string(arena,now,copied))) {
            } else if (OB_FAIL(change.fingerprints.push_back(copied))) {
            } else if (OB_FAIL(change.changed.push_back(now!=old || defs.at(i).get_column_id() == retry_column))) {
            } else { any |= now!=old || defs.at(i).get_column_id() == retry_column; }
          }
          if (OB_SUCC(ret) && any) { ret=changes.push_back(change); }
        }
        if (ret==OB_ITER_END) { ret=OB_SUCCESS; }
      }
    }
  }
  // Remove tasks belonging to rows deleted or moved to a different primary key
  // in this same business transaction. A worker cannot observe this cleanup
  // until the outer transaction commits.
  if (OB_SUCC(ret)) {
    ObSqlString cleanup;
    if (OB_FAIL(cleanup.append_fmt("DELETE FROM oceanbase.__all_ai_column_task WHERE table_id=%lu AND rowkey_predicate NOT IN (SELECT %s FROM %s)", table_id, key_expr.ptr(), qualified.ptr()))) {
    } else { ret = op.execute_write(cleanup.ptr()); }
  }
  static int64_t next_id=0;
  for (int64_t row=0;OB_SUCC(ret) && row<changes.count();++row) {
    auto &change=changes.at(row);
    int64_t request=ATOMIC_AAF(&next_id,1)+ObTimeUtility::current_time()*1000;
    ObSqlString update;
    if (OB_FAIL(update.append_fmt("UPDATE %s SET ",qualified.ptr()))) { break; }
    bool comma=false;
    for (int64_t i=0;OB_SUCC(ret) && i<defs.count();++i) {
      if (!change.changed.at(i)) { continue; }
      const auto &def=defs.at(i);
      const uint64_t ids[]={def.get_column_id(),def.get_state_column_id(),def.get_request_column_id(),def.get_result_spec_column_id()};
      for (int j=0;OB_SUCC(ret) && j<4;++j) {
        if (comma) { ret=update.append(","); }
        const auto *col=table->get_column_schema(ids[j]);
        if (col==nullptr) { ret=OB_ERR_UNEXPECTED; break; }
        if (OB_SUCC(ret)) { ret=quote_name(update,col->get_column_name_str()); }
        if (OB_SUCC(ret)) {
          if (j==0) { ret=update.append("=NULL"); }
          else if (j==1) { ret=update.append("='PENDING'"); }
          else if (j==2) { ret=update.append_fmt("='%ld'",request); }
          else { ret=update.append_fmt("='%.*s'",change.fingerprints.at(i).length(),change.fingerprints.at(i).ptr()); }
        }
        comma=true;
      }
    }
    if (OB_SUCC(ret)) { ret=update.append_fmt(" WHERE %.*s",change.key.length(),change.key.ptr()); }
    if (OB_SUCC(ret)) { ret=op.execute_write(update.ptr()); }
    for (int64_t i=0;OB_SUCC(ret) && i<defs.count();++i) {
      if (!change.changed.at(i)) { continue; }
      const auto &def=defs.at(i); ObSqlString sql, escaped_key;
      if (OB_FAIL(hex_literal(escaped_key,change.key))) { break; }
      if (OB_FAIL(sql.append_fmt("DELETE FROM oceanbase.__all_ai_column_task WHERE table_id=%lu AND column_id=%lu AND rowkey_predicate=%s",table_id,def.get_column_id(),escaped_key.ptr()))) {
      } else if (OB_FAIL(op.execute_write(sql.ptr()))) {
      } else {
        sql.reset();
        if (OB_FAIL(sql.append_fmt("INSERT INTO oceanbase.__all_ai_column_task(table_id,request_id,column_id,rowkey_predicate,state,retry_count,eligible_ts,definition_version) VALUES(%lu,%ld,%lu,%s,0,0,0,%ld)",table_id,request,def.get_column_id(),escaped_key.ptr(),def.get_definition_version()))) {
        } else { ret=op.execute_write(sql.ptr()); }
      }
    }
  }
  const int end_ret=op.end_nested_session();
  if (OB_SUCC(ret)) { ret=end_ret; }
  return ret;
}
} }
