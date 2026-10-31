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

#define USING_LOG_PREFIX SERVER
#include "observer/ai_service/ob_ai_column_worker.h"
#include "sql/engine/dml/ai_column_dml.h"
#include "common/mysqlclient/ob_mysql_transaction.h"
#include "common/mysqlclient/ob_mysql_result.h"
#include "lib/time/ob_time_utility.h"
#include "lib/oblog/ob_log_module.h"
#include <cstdlib>
#include <algorithm>
#include <cmath>
#include <cctype>
namespace oceanbase { namespace observer {
using namespace common;
using namespace common::sqlclient;
namespace {
thread_local const char *result_hint = "";
class ResultHintGuard {
public:
  explicit ResultHintGuard(const std::string &type) : previous_(result_hint) {
    result_hint = type == "json" ? "Return only one valid JSON value, without Markdown or explanation." :
      type.find("int") != std::string::npos ? "Return only one base-10 integer, without Markdown or explanation." :
      (type.find("decimal")==0 || type.find("double")==0 || type.find("float")==0) ?
      "Return only one finite decimal number, without Markdown or explanation." :
      "Return only the requested text, without Markdown or explanation.";
  }
  ~ResultHintGuard() { result_hint=previous_; }
private: const char *previous_;
};
const char *TASK = "oceanbase.__all_ai_column_task";
std::string number(int64_t n) { return std::to_string(n); }
int64_t integer(const std::string &s) { return std::strtoll(s.c_str(), nullptr, 10); }
}
const char *ai_column_result_hint() { return result_hint; }
AiColumnWorker::AiColumnWorker() : proxy_(nullptr), owner_(0) {}
std::string AiColumnWorker::quote(const std::string &s) {
  std::string q="`"; for(char c:s) { q+=c; if(c=='`') q+='`'; } return q+'`';
}
std::string AiColumnWorker::literal(const std::string &s) {
  static const char h[]="0123456789abcdef"; std::string q="CONVERT(X'";
  for(unsigned char c:s) { q+=h[c>>4]; q+=h[c&15]; } return q+"' USING utf8mb4)";
}
int AiColumnWorker::start(ObMySQLProxy &p) {
  int ret=OB_SUCCESS; proxy_=&p; owner_=ObTimeUtility::current_time();
  if (OB_FAIL(timer_.init("AiColumnWorker", ObMemAttr("AiColumnWorker")))) {}
  else if (OB_FAIL(timer_.schedule(*this, 100000, true))) { timer_.destroy(); }
  return ret;
}
void AiColumnWorker::stop() { timer_.stop(); }
void AiColumnWorker::wait() { timer_.wait(); }
void AiColumnWorker::destroy() { timer_.destroy(); proxy_=nullptr; }
int AiColumnWorker::read(ObISQLClient &c, const std::string &sql,
                       const std::vector<std::string> &names, Rows &rows) {
  int ret=OB_SUCCESS; ObMySQLProxy::MySQLResult result;
  if (OB_FAIL(c.read(result, sql.c_str()))) return ret;
  ObMySQLResult *r=result.get_result(); if(!r) return OB_ERR_UNEXPECTED;
  while(OB_SUCC(ret) && OB_SUCC(ret = r->next())) {
    Row row;
    for(const auto &name:names) {
      ObString value;
      if (OB_FAIL(r->get_varchar(name.c_str(), value))) {
        if (ret == OB_ERR_NULL_VALUE) { ret=OB_SUCCESS; row[name+"_null"]="1"; }
        else break;
      }
      row[name]=value.length() > 0 ? std::string(value.ptr(), value.length()) : std::string();
    }
    if(OB_SUCC(ret)) rows.push_back(std::move(row));
  }
  return ret==OB_ITER_END ? OB_SUCCESS : ret;
}
int AiColumnWorker::write(ObISQLClient &c,const std::string &sql,int64_t &n) {
  return c.write(sql.c_str(), n);
}
int AiColumnWorker::metadata(const std::string &table,const std::string &column,Row &row) {
  Rows rows;
  const std::string sql="SELECT d.database_name db,t.table_name tbl,c.column_name col,"
    "s.column_name st,r.column_name req,a.normalized_expr expr,LOWER(i.column_type) typ,"
    "CAST(a.definition_version AS CHAR) ver,CAST(a.max_retries AS CHAR) retries "
    "FROM oceanbase.__all_ai_column a JOIN oceanbase.__all_table t ON t.table_id=a.table_id "
    "JOIN oceanbase.__all_database d ON d.database_id=t.database_id "
    "JOIN oceanbase.__all_column c ON c.table_id=a.table_id AND c.column_id=a.column_id "
    "JOIN oceanbase.__all_column s ON s.table_id=a.table_id AND s.column_id=a.state_column_id "
    "JOIN oceanbase.__all_column r ON r.table_id=a.table_id AND r.column_id=a.request_column_id "
    "JOIN information_schema.columns i ON i.table_schema=d.database_name AND i.table_name=t.table_name AND i.column_name=c.column_name "
    "WHERE a.table_id="+table+" AND a.column_id="+column;
  int ret=read(*proxy_,sql,{"db","tbl","col","st","req","expr","typ","ver","retries"},rows);
  if(ret==OB_SUCCESS) { if(rows.empty()) ret=OB_ENTRY_NOT_EXIST; else row=rows[0]; } return ret;
}
void AiColumnWorker::runTimerTask() {
  if(!proxy_) return;
  sql::AiColumnInternalSQLGuard guard;
  Rows groups;
  int ret=read(*proxy_,std::string("SELECT CAST(table_id AS CHAR) t,CAST(request_id AS CHAR) r FROM ")+TASK+
    " GROUP BY table_id,request_id HAVING SUM(state NOT IN(2,4))=0 LIMIT 16",{"t","r"},groups);
  if(ret==OB_SUCCESS) for(auto &g:groups) (void)publish(g["t"],g["r"]);
  ret=process_one();
  if(ret!=OB_SUCCESS) LOG_WARN("AI column worker iteration failed", K(ret));
}
int AiColumnWorker::process_one() {
  int ret=OB_SUCCESS; ObMySQLTransaction tr; Rows tasks; int64_t n=0;
  const int64_t now=ObTimeUtility::current_time();
  if(OB_FAIL(tr.start(proxy_))) return ret;
  std::string sql=std::string("SELECT CAST(table_id AS CHAR) t,CAST(request_id AS CHAR) r,")+
    "CAST(column_id AS CHAR) c,CAST(definition_version AS CHAR) v,"
    "CAST(claim_generation AS CHAR) g,CAST(retry_count AS CHAR) retry,rowkey_predicate pk FROM "+TASK+
    " WHERE (state IN(0,3) AND eligible_ts<="+number(now)+") OR (state=1 AND lease_ts<"+number(now)+")"
    " ORDER BY eligible_ts LIMIT 1 FOR UPDATE";
  ret=read(tr,sql,{"t","r","c","v","g","retry","pk"},tasks);
  if(ret!=OB_SUCCESS || tasks.empty()) { (void)tr.end(false); return ret; }
  Row task=tasks[0]; const std::string gen=number(integer(task["g"])+1);
  const std::string key="table_id="+task["t"]+" AND request_id="+task["r"]+" AND column_id="+task["c"];
  ret=write(tr,std::string("UPDATE ")+TASK+" SET state=1,owner_id="+number(owner_)+
    ",claim_generation="+gen+",lease_ts="+number(now+120000000)+" WHERE "+key,n);
  int end=tr.end(ret==OB_SUCCESS); if(ret==OB_SUCCESS) ret=end;
  if(ret!=OB_SUCCESS) return ret;
  Row meta; Rows result;
  ret=metadata(task["t"],task["c"],meta);
  if(ret==OB_SUCCESS && meta["ver"]!=task["v"]) ret=OB_ENTRY_NOT_EXIST;
  if(ret==OB_SUCCESS) {
    sql="SELECT CAST(("+meta["expr"]+") AS CHAR) val FROM "+quote(meta["db"])+"."+quote(meta["tbl"])+
      " WHERE ("+task["pk"]+") AND "+quote(meta["req"])+"="+literal(task["r"]);
    ResultHintGuard hint(meta["typ"]);
    ret=read(*proxy_,sql,{"val"},result);
    if(ret==OB_SUCCESS && result.empty()) ret=OB_ENTRY_NOT_EXIST;
  }
  if (ret==OB_SUCCESS && result[0]["val_null"]!="1") {
    const std::string &value=result[0]["val"];
    const std::string &type=meta["typ"];
    if (type.find("int")!=std::string::npos || type.find("decimal")==0 || type.find("double")==0 || type.find("float")==0) {
      char *end=nullptr; const long double numeric=std::strtold(value.c_str(), &end);
      while(end && std::isspace(static_cast<unsigned char>(*end))) ++end;
      if (!end || end==value.c_str() || *end || !std::isfinite(numeric)) ret=OB_INVALID_ARGUMENT;
      else if (type.find("int")!=std::string::npos) {
        int bits = type.find("tinyint")==0 ? 8 : type.find("smallint")==0 ? 16 :
          type.find("mediumint")==0 ? 24 : type.find("bigint")==0 ? 64 : 32;
        const bool unsign=type.find("unsigned")!=std::string::npos;
        const long double bound=std::ldexp(1.0L, unsign ? bits : bits-1);
        if(std::trunc(numeric)!=numeric || numeric >= bound || numeric < (unsign ? 0.0L : -bound)) ret=OB_INVALID_ARGUMENT;
      }
    }
    if (ret==OB_SUCCESS && (type.find("varchar(")==0 || type.find("char(")==0)) {
      const size_t pos=type.find('(');
      Rows checked;
      ret=read(*proxy_,"SELECT CAST(CHAR_LENGTH(CONVERT("+literal(value)+" USING utf8mb4)) AS CHAR) len",{"len"},checked);
      if(ret==OB_SUCCESS && (checked.empty() || integer(checked[0]["len"])>integer(type.substr(pos+1)))) ret=OB_INVALID_ARGUMENT;
    }
    if (ret==OB_SUCCESS && (type=="json" || type.find("vector")==0)) {
      Rows checked;
      ret=read(*proxy_,"SELECT CAST(JSON_VALID("+literal(value)+") AS CHAR) valid",{"valid"},checked);
      if(ret==OB_SUCCESS && (checked.empty() || checked[0]["valid"]!="1")) ret=OB_INVALID_ARGUMENT;
      if(ret==OB_SUCCESS && type.find("vector")==0) {
        checked.clear();
        ret=read(*proxy_,"SELECT JSON_TYPE("+literal(value)+") kind,CAST(JSON_LENGTH("+literal(value)+") AS CHAR) dim",{"kind","dim"},checked);
        const size_t pos=type.find('('); const int64_t dim=pos==std::string::npos?0:integer(type.substr(pos+1));
        if(ret==OB_SUCCESS && (checked.empty() || checked[0]["kind"]!="ARRAY" || integer(checked[0]["dim"])!=dim)) ret=OB_INVALID_ARGUMENT;
        if(ret==OB_SUCCESS) {
          const char *cursor=value.c_str();
          auto whitespace=[&cursor]() { while(std::isspace(static_cast<unsigned char>(*cursor))) ++cursor; };
          whitespace(); if(*cursor!='[') ret=OB_INVALID_ARGUMENT; else ++cursor;
          int64_t actual=0;
          while(ret==OB_SUCCESS) {
            whitespace(); if(*cursor==']') { ++cursor; break; }
            char *next=nullptr; const long double element=std::strtold(cursor,&next);
            if(next==cursor || !std::isfinite(element) || std::fabs(element)>3.402823466e38L) { ret=OB_INVALID_ARGUMENT; break; }
            cursor=next; ++actual; whitespace();
            if(*cursor==',') ++cursor;
            else if(*cursor!=']') { ret=OB_INVALID_ARGUMENT; break; }
          }
          whitespace(); if(ret==OB_SUCCESS && (*cursor || actual!=dim)) ret=OB_INVALID_ARGUMENT;
        }
      }
    }
  }
  const std::string fence=key+" AND owner_id="+number(owner_)+" AND claim_generation="+gen+" AND state=1";
  if(ret==OB_ENTRY_NOT_EXIST) return write(*proxy_,std::string("DELETE FROM ")+TASK+" WHERE "+fence,n);
  if(ret==OB_SUCCESS) {
    ret=write(*proxy_,std::string("UPDATE ")+TASK+" SET state=2,result_cache="+literal(result[0]["val"])+
      ",result_type="+literal(result[0]["val_null"]=="1"?"null":"sql")+",lease_ts=0 WHERE "+fence,n);
  } else {
    const int64_t retry=integer(task["retry"])+1;
    const bool again=retry<=integer(meta["retries"]);
    sql=std::string("UPDATE ")+TASK+" SET state="+(again?"3":"4")+",retry_count="+number(retry)+
      ",eligible_ts="+number(ObTimeUtility::current_time()+1000000*(1LL<<std::min<int64_t>(retry-1,6)))+
      ",lease_ts=0,last_error="+literal("AI evaluation error "+number(ret))+" WHERE "+fence;
    ret=write(*proxy_,sql,n);
  }
  if(ret==OB_SUCCESS) ret=publish(task["t"],task["r"]);
  return ret;
}
int AiColumnWorker::publish(const std::string &table,const std::string &request) {
  int ret=OB_SUCCESS; ObMySQLTransaction tr; Rows tasks; int64_t n=0;
  if(OB_FAIL(tr.start(proxy_))) return ret;
  std::string group="table_id="+table+" AND request_id="+request;
  ret=read(tr,std::string("SELECT CAST(column_id AS CHAR) c,CAST(state AS CHAR) s,")+
    "CAST(definition_version AS CHAR) v,rowkey_predicate pk,IFNULL(result_cache,'') val,IFNULL(result_type,'') typ FROM "+TASK+
    " WHERE "+group+" ORDER BY column_id FOR UPDATE",{"c","s","v","pk","val","typ"},tasks);
  bool terminal=!tasks.empty();
  for(auto &task:tasks) if(task["s"]!="2" && task["s"]!="4") terminal=false;
  if(ret==OB_SUCCESS && terminal) {
    for(auto &task:tasks) {
      // Fence DDL changes as well as row requests: the definition lock remains
      // held through the user-row update and task removal in this transaction.
      Rows locked;
      ret=read(tr,"SELECT CAST(definition_version AS CHAR) ver FROM oceanbase.__all_ai_column WHERE table_id="+
        table+" AND column_id="+task["c"]+" FOR UPDATE",{"ver"},locked);
      if(ret!=OB_SUCCESS) break;
      if(locked.empty() || locked[0]["ver"]!=task["v"]) continue;
      Row meta; ret=metadata(table,task["c"],meta);
      if(ret==OB_ENTRY_NOT_EXIST) { ret=OB_SUCCESS; continue; }
      if(ret!=OB_SUCCESS) break;
      if(meta["ver"]!=task["v"]) continue;
      std::string sql="UPDATE "+quote(meta["db"])+"."+quote(meta["tbl"])+" SET "+quote(meta["col"])+
        "="+(task["s"]=="2" && task["typ"]!="null"?literal(task["val"]):"NULL")+","+quote(meta["st"])+
        "="+literal(task["s"]=="2"?"READY":"FAILED")+" WHERE ("+task["pk"]+") AND "+quote(meta["req"])+"="+literal(request);
      if(OB_SUCCESS!=(ret=write(tr,sql,n))) break;
    }
    if(ret==OB_SUCCESS) ret=write(tr,std::string("DELETE FROM ")+TASK+" WHERE "+group,n);
  }
  int end=tr.end(ret==OB_SUCCESS); return ret==OB_SUCCESS?end:ret;
}
} }
