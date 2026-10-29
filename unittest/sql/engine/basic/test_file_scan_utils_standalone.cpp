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

// Standalone correctness checks for the actual file scanner, without a database adapter.
#include "sql/engine/basic/file_scan_utils.h"
#include <cassert>
#include <fstream>
#include <iostream>
using namespace oceanbase::sql::file_sql;
static void write(const std::string &path,const std::string &text){std::ofstream f(path,std::ios::binary);f<<text;}
int main(int argc,char **argv) {
  assert(argc==2);
  Options sample;sample.root=argv[1];sample.path=sample.root+"/basic.csv";
  Reader csv(sample);assert(csv.infer_schema());assert(csv.columns.size()==8);
  assert(csv.columns[7].name=="id__2");assert(csv.columns[2].type==Type::Double);
  assert(csv.columns[3].type==Type::Bool);assert(csv.columns[4].type==Type::Date);
  assert(csv.columns[5].type==Type::DateTime);
  std::vector<Cell> row;assert(csv.next(row));assert(row[6].text=="hello,\nworld");
  assert(csv.next(row));assert(row[4].null);assert(csv.next(row));assert(row[6].text=="say \"hi\"");
  assert(!csv.next(row)&&csv.error.empty());assert(csv.rescan());assert(csv.next(row)&&row[0].text=="1");
  Options json=sample;json.path=json.root+"/basic.jsonl";json.format="jsonl";
  Reader jr(json);assert(jr.infer_schema());assert(jr.columns.size()==6);int count=0;while(jr.next(row))++count;assert(count==3&&jr.error.empty());
  char directory[]="/tmp/file-sql-test-XXXXXX";assert(mkdtemp(directory));std::string root=directory;
  Options o;o.root=root;o.path=root+"/input.csv";
  write(o.path,"v,v\r\n\"\",NULL\r\n\"a\r\nb\",2\r\n");
  Reader crlf(o);assert(crlf.infer_schema());assert(crlf.next(row));assert(!row[0].null&&row[0].text.empty()&&row[1].null);assert(crlf.next(row));assert(row[0].text=="a\r\nb");
  write(o.path,"v\n\"broken\n");Reader bad(o);assert(!bad.infer_schema());
  write(o.path,"v\n1,2\n");Reader width(o);assert(!width.infer_schema());
  write(o.path,"v\n9223372036854775808\n");Reader big(o);assert(big.infer_schema()&&big.columns[0].type==Type::String);
  write(o.path,"v\n2024-02-29\n2023-02-29\n");Reader date(o);assert(date.infer_schema()&&date.columns[0].type==Type::String);
  write(o.path,"v\n1\n");Reader changed(o);assert(changed.infer_schema());write(o.path,"v\n2\n3\n");assert(!changed.next(row)&&!changed.error.empty());
  write(o.path,"v\n1\n");Reader replaced(o);assert(replaced.infer_schema());assert(rename(o.path.c_str(),(o.path+".old").c_str())==0);write(o.path,"v\n2\n");assert(!replaced.next(row));
  Options escape=o;escape.path="/etc/passwd";Reader outside(escape);assert(!outside.infer_schema());
  assert(symlink("/etc/passwd",(root+"/link.csv").c_str())==0);escape.path=root+"/link.csv";Reader link(escape);assert(!link.infer_schema());
  std::map<std::string,Cell> object;
  assert(FlatJson("{\"x\":\"\\ud83d\\ude00\",\"y\":null}").parse(object));assert(object["x"].text=="\xf0\x9f\x98\x80");
  for(const std::string &bad:{"{\"x\":{}}","{\"x\":[1]}","{\"x\":01}","{\"x\":1,\"x\":2}","{\"x\":\"\\ud800\"}"}){object.clear();assert(!FlatJson(bad).parse(object));}
  Options recovered;assert(decode(encode(o),recovered));assert(recovered.path==o.path&&recovered.root==o.root);
  assert(!decode("FILESQL1:99999:a",recovered));assert(!valid_utf8("\xc0\x80"));assert(valid_utf8("中文😀"));
  Options output=o;output.path=root+"/output.csv";
  {CopyWriter writer(output);assert(writer.open());Cell name;name.text="value";assert(writer.row({name}));Cell text;text.text="a,b\nquote\"";assert(writer.row({text}));Cell empty;assert(writer.row({empty}));Cell null;null.null=true;assert(writer.row({null}));assert(writer.commit());}
  Reader roundtrip(output);assert(roundtrip.infer_schema());assert(roundtrip.next(row)&&row[0].text=="a,b\nquote\"");assert(roundtrip.next(row)&&row[0].text.empty()&&!row[0].null);assert(roundtrip.next(row)&&row[0].null);
  {CopyWriter existing(output);assert(!existing.open());}
  Options aborted=output;aborted.path=root+"/aborted.csv";{CopyWriter writer(aborted);assert(writer.open());Cell text;text.text="temporary";assert(writer.row({text}));}
  assert(access(aborted.path.c_str(),F_OK)!=0);
  Options glob=o;glob.format="glob";glob.path=root+"/output*";Reader discovered(glob);assert(discovered.infer_schema());assert(discovered.next(row)&&row[0].text==output.path);assert(!discovered.next(row));assert(discovered.rescan());assert(discovered.next(row));
  unlink(output.path.c_str());
  unlink(o.path.c_str());unlink((o.path+".old").c_str());unlink((root+"/link.csv").c_str());rmdir(root.c_str());
  std::cout<<"File scanner correctness checks passed\n";
}
