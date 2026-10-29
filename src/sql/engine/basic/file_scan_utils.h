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

#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <cmath>
#include <climits>
#include <cstring>
#include <cstdint>
#include <ctime>
#include <limits>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <fnmatch.h>

namespace oceanbase { namespace sql { namespace file_sql {

enum class Type { Null, Int, Double, Bool, String, Date, DateTime };
struct Cell { std::string text; bool null = false; bool quoted = false; };
struct Column { std::string name; Type type = Type::Null; bool nullable = false; };
struct Options {
  std::string path, root, format = "csv", nullstr = "NULL", schema;
  char delimiter = ',', quote = '"', escape = '"';
  bool header = true;
};

inline bool valid_utf8(const std::string &text) {
  for (size_t i=0;i<text.size();) {
    unsigned char c=text[i++];
    if(c<128)continue;
    unsigned count=c>=0xc2&&c<=0xdf?1:c>=0xe0&&c<=0xef?2:c>=0xf0&&c<=0xf4?3:0;
    if(!count||i+count>text.size())return false;
    uint32_t code=c&((1u<<(6-count))-1);
    for(unsigned j=0;j<count;++j){unsigned char next=text[i++];if((next&0xc0)!=0x80)return false;code=(code<<6)|(next&63);}
    if(code>0x10ffff||(code>=0xd800&&code<=0xdfff)||(count==1&&code<0x80)||(count==2&&code<0x800)||(count==3&&code<0x10000))return false;
  } return true;
}

inline bool valid_date(const std::string &s, bool datetime = false)
{
  if (s.size() != (datetime ? 19 : 10)) return false;
  for (size_t i = 0; i < s.size(); ++i) {
    if (i == 4 || i == 7) { if (s[i] != '-') return false; }
    else if (i == 10) { if (s[i] != ' ' && s[i] != 'T') return false; }
    else if (i == 13 || i == 16) { if (s[i] != ':') return false; }
    else if (s[i] < '0' || s[i] > '9') return false;
  }
  auto n = [&](size_t i, size_t count) { return std::atoi(s.substr(i, count).c_str()); };
  int y = n(0,4), m = n(5,2), d = n(8,2);
  const int days[] = {0,31,28,31,30,31,30,31,31,30,31,30,31};
  if (y < 1 || m < 1 || m > 12) return false;
  int max_day = days[m] + (m == 2 && y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
  return d >= 1 && d <= max_day && (!datetime || (n(11,2) < 24 && n(14,2) < 60 && n(17,2) < 60));
}
inline Type infer(const Cell &v)
{
  if (v.null) return Type::Null;
  if (v.text == "true" || v.text == "false" || v.text == "TRUE" || v.text == "FALSE") return Type::Bool;
  if (v.text.empty()) return Type::String;
  if (valid_date(v.text)) return Type::Date;
  if (valid_date(v.text, true)) return Type::DateTime;
  const char *p = v.text.c_str();
  char *end = nullptr;
  if (*p == '+' || *p == '-') ++p;
  bool digits = *p != 0;
  for (; *p; ++p) digits &= *p >= '0' && *p <= '9';
  if (digits) {
    errno = 0; std::strtoll(v.text.c_str(), &end, 10);
    // Preserve integers outside BIGINT exactly rather than round them through DOUBLE.
    return errno == ERANGE ? Type::String : Type::Int;
  }
  errno = 0;
  double value = std::strtod(v.text.c_str(), &end);
  if (end != v.text.c_str() && *end == 0 && errno != ERANGE && std::isfinite(value)
      && v.text.find_first_of(".eE") != std::string::npos) return Type::Double;
  return Type::String;
}
inline Type widen(Type a, Type b)
{
  if (a == Type::Null) return b;
  if (b == Type::Null || a == b) return a;
  if ((a == Type::Int && b == Type::Double) || (b == Type::Int && a == Type::Double)) return Type::Double;
  if ((a == Type::Date && b == Type::DateTime) || (b == Type::Date && a == Type::DateTime)) return Type::DateTime;
  return Type::String;
}
inline std::string lower(std::string value)
{
  for (char &c : value) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
  return value;
}

// Opening each component beneath an already-open secure root prevents both
// symlink escape and replacement of a parent directory during path validation.
inline int secure_open(const std::string &path, const std::string &root, bool directory = false)
{
  char real_root[PATH_MAX], real_file[PATH_MAX];
  if (root.empty() || !realpath(root.c_str(), real_root) || !realpath(path.c_str(), real_file)) return -1;
  std::string base(real_root), file(real_file);
  if (base != "/" && file != base && file.compare(0, base.size() + 1, base + "/") != 0) { errno = EACCES; return -1; }
  std::string relative = file == base ? std::string() : file.substr(base == "/" ? 1 : base.size() + 1);
  int fd = open(base.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) return -1;
  if (file == base) { if (directory) return fd; close(fd); errno = EISDIR; return -1; }
  size_t pos = 0;
  while (pos < relative.size()) {
    size_t end = relative.find('/', pos);
    bool last = end == std::string::npos;
    std::string name = relative.substr(pos, last ? end : end - pos);
    int next = openat(fd, name.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | ((!last || directory) ? O_DIRECTORY : O_NONBLOCK));
    close(fd); fd = next;
    if (fd < 0) return -1;
    if (last) break;
    pos = end + 1;
  }
  struct stat st;
  if (fstat(fd, &st) || (directory ? !S_ISDIR(st.st_mode) : !S_ISREG(st.st_mode))) { close(fd); errno = EACCES; return -1; }
  return fd;
}

inline void pack_string(std::string &out, const std::string &value) {
  out += std::to_string(value.size()) + ":" + value;
}
inline bool unpack_string(const std::string &in, size_t &pos, std::string &value) {
  size_t colon=in.find(':',pos); if(colon==std::string::npos||colon==pos||colon-pos>9)return false;
  size_t size=0;for(size_t i=pos;i<colon;++i){if(in[i]<'0'||in[i]>'9')return false;size=size*10+in[i]-'0';}
  pos=colon+1;if(size>in.size()-pos)return false;value=in.substr(pos,size);pos+=size;return true;
}
inline std::string schema_descriptor(const std::vector<Column> &columns) {
  std::string out;
  for(const auto &column:columns){pack_string(out,column.name);out+=char('0'+int(column.type));out+=column.nullable?'1':'0';}
  return out;
}
inline bool schema_names(const std::string &schema,std::vector<std::string> &names) {
  size_t pos=0;
  while(pos<schema.size()){
    std::string name;if(!unpack_string(schema,pos,name)||pos+2>schema.size())return false;
    if(schema[pos]<'0'||schema[pos]>'6'||(schema[pos+1]!='0'&&schema[pos+1]!='1'))return false;
    pos+=2;names.push_back(name);
  }
  return !names.empty();
}
inline std::string encode(const Options &o) {
  std::string out="FILESQL1:";
  for(const auto &s:{o.path,o.root,o.format,o.nullstr,o.schema})pack_string(out,s);
  out+=o.delimiter;out+=o.quote;out+=o.escape;out+=o.header?'1':'0';return out;
}
inline bool decode(const std::string &in, Options &o) {
  if(in.compare(0,9,"FILESQL1:")!=0)return false;
  size_t pos=9;
  if(!unpack_string(in,pos,o.path)||!unpack_string(in,pos,o.root)||!unpack_string(in,pos,o.format)||!unpack_string(in,pos,o.nullstr)||!unpack_string(in,pos,o.schema)||pos+4!=in.size())return false;
  o.delimiter=in[pos++];o.quote=in[pos++];o.escape=in[pos++];o.header=in[pos]=='1';return true;
}

class FlatJson {
public:
  explicit FlatJson(const std::string &s) : s_(s) {}
  bool parse(std::map<std::string, Cell> &out)
  {
    ws(); if (!take('{')) return false; ws();
    if (take('}')) { ws(); return p_ == s_.size(); }
    do {
      std::string key; if (!string(key)) return false;
      ws(); if (!take(':')) return false; ws();
      Cell cell;
      if (p_ < s_.size() && s_[p_] == '"') { cell.quoted = true; if (!string(cell.text)) return false; }
      else {
        size_t start = p_;
        while (p_ < s_.size() && s_[p_] != ',' && s_[p_] != '}' && s_[p_] != ' ' && s_[p_] != '\t' && s_[p_] != '\r') ++p_;
        cell.text = s_.substr(start, p_ - start);
        cell.null = cell.text == "null";
        if (!cell.null && cell.text != "true" && cell.text != "false" && !number(cell.text)) return false;
      }
      if (out.count(key)) return false;
      out.emplace(key, cell);
      if (out.size() > 1024) return false;
      ws(); if (take('}')) { ws(); return p_ == s_.size(); }
      if (!take(',')) return false;
      ws();
    } while (p_ < s_.size());
    return false;
  }
private:
  bool number(const std::string &v) {
    size_t i=0; if (i<v.size() && v[i]=='-') ++i;
    if (i==v.size()) return false;
    if (v[i]=='0') ++i;
    else { if (v[i]<'1'||v[i]>'9') return false; while(i<v.size()&&v[i]>='0'&&v[i]<='9')++i; }
    if(i<v.size()&&v[i]=='.'){size_t start=++i;while(i<v.size()&&v[i]>='0'&&v[i]<='9')++i;if(start==i)return false;}
    if(i<v.size()&&(v[i]=='e'||v[i]=='E')){++i;if(i<v.size()&&(v[i]=='+'||v[i]=='-'))++i;size_t start=i;while(i<v.size()&&v[i]>='0'&&v[i]<='9')++i;if(start==i)return false;}
    return i==v.size();
  }
  void ws() { while (p_ < s_.size() && (s_[p_]==' ' || s_[p_]=='\t' || s_[p_]=='\r')) ++p_; }
  bool take(char c) { if (p_ < s_.size() && s_[p_] == c) { ++p_; return true; } return false; }
  bool hex4(uint32_t &v) {
    v=0; for(int i=0;i<4;++i){if(p_==s_.size())return false;char c=s_[p_++];int d=c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:-1;if(d<0)return false;v=v*16+d;} return true;
  }
  void utf8(std::string &out,uint32_t c){
    if(c<0x80)out+=char(c);else if(c<0x800){out+=char(0xc0|(c>>6));out+=char(0x80|(c&63));}
    else if(c<0x10000){out+=char(0xe0|(c>>12));out+=char(0x80|((c>>6)&63));out+=char(0x80|(c&63));}
    else{out+=char(0xf0|(c>>18));out+=char(0x80|((c>>12)&63));out+=char(0x80|((c>>6)&63));out+=char(0x80|(c&63));}
  }
  bool string(std::string &out) {
    ws(); if(!take('"'))return false;
    while(p_<s_.size()) {
      unsigned char c=s_[p_++]; if(c=='"')return true; if(c<32)return false;
      if(c!='\\'){out+=char(c);continue;}
      if(p_==s_.size())return false;
      char e=s_[p_++];
      switch(e){case '"':case '\\':case '/':out+=e;break;case 'b':out+='\b';break;case 'f':out+='\f';break;case 'n':out+='\n';break;case 'r':out+='\r';break;case 't':out+='\t';break;
      case 'u':{uint32_t v;if(!hex4(v))return false;if(v>=0xd800&&v<=0xdbff){if(!take('\\')||!take('u'))return false;uint32_t lo;if(!hex4(lo)||lo<0xdc00||lo>0xdfff)return false;v=0x10000+((v-0xd800)<<10)+(lo-0xdc00);}else if(v>=0xdc00&&v<=0xdfff)return false;utf8(out,v);break;}
      default:return false;}
    } return false;
  }
  const std::string &s_; size_t p_=0;
};


// Minimal bounded Parquet reader.  SQL execution remains in the seekdb executor;
// this decoder only implements the file container and primitive column pages.
struct CompactValue {
  int64_t integer=0; std::string bytes;
  std::map<int,CompactValue> fields; std::vector<CompactValue> list;
  const CompactValue &field(int id) const {static const CompactValue empty;auto it=fields.find(id);return it==fields.end()?empty:it->second;}
};
class CompactReader {
public:
  explicit CompactReader(const std::string &bytes):data(bytes){}
  size_t position=0;
  bool object(CompactValue &out,int depth=0) {
    if(depth>32)return false;
    int last=0;
    while(position<data.size()){
      unsigned byte=static_cast<unsigned char>(data[position++]);if(byte==0)return true;
      int type=byte&15,delta=byte>>4;uint64_t field=0;
      if(delta)last+=delta;
      else{if(!varint(field)||field>65535)return false;last=static_cast<int>((field>>1)^-static_cast<int64_t>(field&1));}
      if(last<=0||out.fields.size()>=1024)return false;
      CompactValue value;if(!read(type,value,depth+1))return false;out.fields[last]=std::move(value);
    }return false;
  }
private:
  bool varint(uint64_t &value){value=0;for(unsigned bits=0;bits<64;bits+=7){if(position==data.size())return false;unsigned c=static_cast<unsigned char>(data[position++]);if(bits==63&&(c&0xfe))return false;value|=uint64_t(c&127)<<bits;if(!(c&128))return true;}return false;}
  bool read(int type,CompactValue &out,int depth){
    uint64_t n=0;
    switch(type){
      case 1:out.integer=1;return true;case 2:out.integer=0;return true;
      case 3:if(position==data.size())return false;out.integer=static_cast<signed char>(data[position++]);return true;
      case 4:case 5:case 6:if(!varint(n))return false;out.integer=static_cast<int64_t>((n>>1)^-static_cast<int64_t>(n&1));return true;
      case 7:if(data.size()-position<8)return false;out.bytes=data.substr(position,8);position+=8;return true;
      case 8:if(!varint(n)||n>16*1024*1024||n>data.size()-position)return false;out.bytes=data.substr(position,n);position+=n;return true;
      case 9:case 10:{if(position==data.size()||depth>32)return false;unsigned h=static_cast<unsigned char>(data[position++]);n=h>>4;if(n==15&&!varint(n))return false;if(n>1000000)return false;for(uint64_t i=0;i<n;++i){CompactValue item;if(!read(h&15,item,depth+1))return false;out.list.push_back(std::move(item));}return true;}
      case 12:return object(out,depth+1);
      default:return false;
    }
  }
  const std::string &data;
};
inline bool unsigned_varint(const std::string &data,size_t &pos,uint64_t &value){
  value=0;for(unsigned bits=0;bits<64;bits+=7){if(pos==data.size())return false;unsigned byte=static_cast<unsigned char>(data[pos++]);if(bits==63&&(byte&0xfe))return false;value|=uint64_t(byte&127)<<bits;if(!(byte&128))return true;}return false;
}
inline bool little(const std::string &data,size_t &pos,unsigned bytes,uint64_t &value){
  if(bytes>8||bytes>data.size()-pos)return false;value=0;for(unsigned i=0;i<bytes;++i)value|=uint64_t(static_cast<unsigned char>(data[pos++]))<<(8*i);return true;
}
inline bool snappy(const std::string &input,std::string &output){
  size_t pos=0;uint64_t target=0;
  if(!unsigned_varint(input,pos,target)||target>16*1024*1024)return false;
  output.clear();output.reserve(target);
  while(pos<input.size()&&output.size()<target){
    unsigned tag=static_cast<unsigned char>(input[pos++]),kind=tag&3;uint64_t length=0,offset=0;
    if(kind==0){unsigned code=tag>>2;if(code<60)length=code+1;else{if(!little(input,pos,code-59,length))return false;++length;}
      if(length>target-output.size()||length>input.size()-pos)return false;output.append(input,pos,length);pos+=length;
    }else{
      if(kind==1){if(pos==input.size())return false;length=4+((tag>>2)&7);offset=((tag&0xe0)<<3)|static_cast<unsigned char>(input[pos++]);}
      else{length=1+(tag>>2);if(!little(input,pos,kind==2?2:4,offset))return false;}
      if(offset==0||offset>output.size()||length>target-output.size())return false;
      for(uint64_t i=0;i<length;++i)output+=output[output.size()-offset];
    }
  }return pos==input.size()&&output.size()==target;
}
inline bool hybrid(const std::string &data,size_t &pos,unsigned width,size_t count,std::vector<uint32_t> &out){
  if(width>32||count>1000000)return false;out.clear();out.reserve(count);
  while(out.size()<count){
    uint64_t run=0;if(!unsigned_varint(data,pos,run)||run<2)return false;
    if((run&1)==0){uint64_t value=0;if(!little(data,pos,(width+7)/8,value))return false;uint64_t n=run>>1;if(n>1000000)return false;for(uint64_t i=0;i<n&&out.size()<count;++i)out.push_back(value);}
    else{uint64_t n=(run>>1)*8;if(n>1000008)return false;uint64_t bytes=(n*width+7)/8;if(bytes>data.size()-pos)return false;size_t begin=pos;pos+=bytes;
      for(uint64_t i=0;i<n&&out.size()<count;++i){uint32_t value=0;for(unsigned b=0;b<width;++b){size_t bit=i*width+b;value|=uint32_t((static_cast<unsigned char>(data[begin+bit/8])>>(bit%8))&1)<<b;}out.push_back(value);}
    }
  }return true;
}
inline std::string parquet_time(int64_t microseconds,bool date_only){
  int64_t seconds=microseconds/1000000,remainder=microseconds%1000000;
  if(remainder<0){remainder+=1000000;--seconds;}
  time_t stamp=seconds;struct tm value{};
  if(!gmtime_r(&stamp,&value)||value.tm_year+1900<1||value.tm_year+1900>9999)return {};
  char text[64];
  if(date_only)snprintf(text,sizeof(text),"%04d-%02d-%02d",value.tm_year+1900,value.tm_mon+1,value.tm_mday);
  else if(remainder)snprintf(text,sizeof(text),"%04d-%02d-%02d %02d:%02d:%02d.%06lld",value.tm_year+1900,value.tm_mon+1,value.tm_mday,value.tm_hour,value.tm_min,value.tm_sec,static_cast<long long>(remainder));
  else snprintf(text,sizeof(text),"%04d-%02d-%02d %02d:%02d:%02d",value.tm_year+1900,value.tm_mon+1,value.tm_mday,value.tm_hour,value.tm_min,value.tm_sec);
  return text;
}
class ParquetReader {
public:
  std::string error;
  std::vector<Column> columns;
  explicit ParquetReader(int fd,int64_t size):fd_(fd),size_(size){}
  bool initialize(){
    std::string start,tail,footer;
    if(size_<12||!read(0,4,start)||start!="PAR1"||!read(size_-8,8,tail)||tail.substr(4)!="PAR1")return fail("invalid Parquet magic");
    size_t pos=0;uint64_t length=0;little(tail,pos,4,length);
    if(length>16*1024*1024||length>uint64_t(size_-12)||!read(size_-8-length,length,footer))return fail("invalid Parquet footer length");
    if(!CompactReader(footer).object(metadata_))return fail("invalid Parquet footer");
    const auto &schema=metadata_.field(2).list;
    if(schema.empty()||schema.size()>1025||schema[0].field(5).integer!=static_cast<int64_t>(schema.size()-1))return fail("nested Parquet schema is unsupported");
    for(size_t i=1;i<schema.size();++i){
      if(schema[i].field(5).integer||schema[i].field(3).integer==2||!schema[i].fields.count(1))return fail("nested or repeated Parquet column is unsupported");
      int physical=schema[i].field(1).integer,converted=schema[i].fields.count(6)?schema[i].field(6).integer:-1;
      Type type=physical==0?Type::Bool:(physical==1||physical==2)?Type::Int:(physical==4||physical==5)?Type::Double:Type::String;
      if(converted==6)type=Type::Date;
      if(converted==9||converted==10||physical==3)type=Type::DateTime;
      if(physical<0||physical>7||schema[i].field(4).bytes.empty())return fail("invalid Parquet column schema");
      for(const auto &c:columns)if(lower(c.name)==lower(schema[i].field(4).bytes))return fail("duplicate Parquet column name");
      columns.push_back({schema[i].field(4).bytes,type,schema[i].field(3).integer==1});
    }
    if(metadata_.field(4).list.size()>100000)return fail("too many Parquet row groups");
    return reset();
  }
  void projection(std::vector<size_t> ids){projection_=std::move(ids);projection_set_=true;}
  bool reset(){group_=0;row_=0;loaded_=false;cursors_.clear();return true;}
  bool next(std::vector<Cell> &out){
    const auto &groups=metadata_.field(4).list;
    while(group_<groups.size()){
      int64_t rows=groups[group_].field(3).integer;
      if(rows<0)return fail("invalid Parquet row count");
      if(row_>=rows){++group_;row_=0;loaded_=false;cursors_.clear();continue;}
      if(!loaded_&&!load_group(groups[group_]))return false;
      out.assign(columns.size(),Cell());for(auto &v:out)v.null=true;
      for(size_t index:projection_){
        if(index>=cursors_.size())return fail("invalid projected Parquet column");
        Cursor &cursor=cursors_[index];
        while(cursor.position==cursor.values.size()){if(!page(cursor,index))return false;}
        out[index]=cursor.values[cursor.position++];
      }
      ++row_;return true;
    }return false;
  }
private:
  struct Cursor {int64_t offset=0,end=0;int codec=0;std::vector<Cell> dictionary,values;size_t position=0;};
  bool fail(const char *message){error=message;return false;}
  bool read(int64_t offset,size_t length,std::string &out){
    if(offset<0||length>16*1024*1024||offset>size_||length>static_cast<uint64_t>(size_-offset))return false;
    out.resize(length);size_t got=0;
    while(got<length){ssize_t n=pread(fd_,&out[got],length-got,offset+got);if(n<=0)return false;got+=n;}return true;
  }
  bool load_group(const CompactValue &group){
    const auto &chunks=group.field(1).list;
    if(chunks.size()!=columns.size())return fail("Parquet row group schema mismatch");
    cursors_.resize(columns.size());
    for(size_t i=0;i<chunks.size();++i){
      const auto &meta=chunks[i].field(3);Cursor &cursor=cursors_[i];
      cursor.offset=meta.field(9).integer;
      if(meta.fields.count(11))cursor.offset=std::min(cursor.offset,meta.field(11).integer);
      int64_t size=meta.field(7).integer;
      if(cursor.offset<4||size<0||size>size_-cursor.offset)return fail("invalid Parquet column bounds");
      cursor.end=cursor.offset+size;cursor.codec=meta.field(4).integer;
      if(cursor.codec!=0&&cursor.codec!=1)return fail("unsupported Parquet compression codec");
    }
    if(!projection_set_){for(size_t i=0;i<columns.size();++i)projection_.push_back(i);projection_set_=true;}
    loaded_=true;return true;
  }
  bool plain(const std::string &bytes,size_t &pos,size_t count,size_t col,std::vector<Cell> &values){
    const auto &schema=metadata_.field(2).list[col+1];
    int physical=schema.field(1).integer,converted=schema.fields.count(6)?schema.field(6).integer:-1;
    if(count>1000000)return false;values.clear();values.reserve(count);
    for(size_t i=0;i<count;++i){
      Cell cell;uint64_t raw=0;char text[96];
      if(physical==0){if(pos+i/8>=bytes.size())return false;cell.text=(bytes[pos+i/8]&(1<<(i%8)))?"true":"false";}
      else if(physical==1||physical==2){
        if(!little(bytes,pos,physical==1?4:8,raw))return false;
        int64_t value=physical==1?static_cast<int32_t>(raw):static_cast<int64_t>(raw);
        if(converted==6){if(value< -719162||value>2932896)return false;cell.text=parquet_time(value*86400000000LL,true);}
        else if(converted==9||converted==10){if(converted==9&&(value>INT64_MAX/1000||value<INT64_MIN/1000))return false;cell.text=parquet_time(converted==9?value*1000:value,false);if(cell.text.empty())return false;}
        else cell.text=std::to_string(value);
      }else if(physical==3){
        uint64_t nanos=0,days=0;if(!little(bytes,pos,8,nanos)||!little(bytes,pos,4,days)||nanos>=86400000000000ULL)return false;
        int64_t julian=static_cast<int64_t>(days)-2440588;
        if(julian< -719162||julian>2932896)return false;cell.text=parquet_time(julian*86400000000LL+nanos/1000,false);
      }else if(physical==4||physical==5){
        if(!little(bytes,pos,physical==4?4:8,raw))return false;
        double value;if(physical==4){uint32_t bits=raw;float f;std::memcpy(&f,&bits,4);value=f;}else std::memcpy(&value,&raw,8);
        if(!std::isfinite(value))return false;
        snprintf(text,sizeof(text),physical==4?"%.9g":"%.17g",value);cell.text=text;
      }else{
        uint64_t length=schema.field(2).integer;
        if(physical==6&&!little(bytes,pos,4,length))return false;
        if(length>bytes.size()-pos)return false;cell.text=bytes.substr(pos,length);pos+=length;
        if(!valid_utf8(cell.text))return false;
      }
      values.push_back(std::move(cell));
    }
    if(physical==0)pos+=(count+7)/8;
    return true;
  }
  bool page(Cursor &cursor,size_t col){
    cursor.values.clear();cursor.position=0;
    if(cursor.offset>=cursor.end)return fail("Parquet column ended before its row group");
    std::string header_bytes;
    if(!read(cursor.offset,std::min<int64_t>(65536,size_-cursor.offset),header_bytes))return fail("cannot read Parquet page header");
    CompactValue header;CompactReader compact(header_bytes);
    if(!compact.object(header))return fail("invalid Parquet page header");
    int64_t compressed=header.field(3).integer,uncompressed=header.field(2).integer;
    cursor.offset+=compact.position;
    if(compressed<0||compressed>16*1024*1024||uncompressed<0||uncompressed>16*1024*1024||compressed>cursor.end-cursor.offset)return fail("Parquet page exceeds resource or file limits");
    std::string stored,bytes;
    if(!read(cursor.offset,compressed,stored))return fail("cannot read Parquet page");cursor.offset+=compressed;
    int page_type=header.field(1).integer;
    if(page_type!=0&&page_type!=2&&page_type!=3)return true;
    size_t data_pos=0,level_size=0;
    if(page_type==3){
      const auto &v2=header.field(8);int64_t defs=v2.field(5).integer,reps=v2.field(6).integer;
      if(defs<0||reps!=0||defs>compressed)return fail("invalid Parquet V2 level lengths");level_size=defs;
      if(cursor.codec==1&&(!v2.fields.count(7)||v2.field(7).integer)){
        std::string decoded;if(!snappy(stored.substr(level_size),decoded))return fail("invalid Snappy page");bytes=stored.substr(0,level_size)+decoded;
      }else bytes=stored;
    }else if(cursor.codec==1){if(!snappy(stored,bytes))return fail("invalid Snappy page");}
    else bytes=stored;
    if(static_cast<int64_t>(bytes.size())!=uncompressed)return fail("Parquet decompressed size mismatch");
    if(page_type==2){
      const auto &dict=header.field(7);if(dict.field(2).integer!=0||dict.field(1).integer<0||!plain(bytes,data_pos,dict.field(1).integer,col,cursor.dictionary))return fail("unsupported or invalid Parquet dictionary");return true;
    }
    const auto &data_header=header.field(page_type==3?8:5);
    int64_t count=data_header.field(1).integer,encoding=data_header.field(page_type==3?4:2).integer;
    if(count<=0||count>1000000)return fail("invalid Parquet data page count");
    const auto &schema=metadata_.field(2).list[col+1];bool optional=schema.field(3).integer==1;
    std::vector<uint32_t> definitions;
    if(optional){
      uint64_t levels_length=level_size;
      if(page_type==0&&!little(bytes,data_pos,4,levels_length))return fail("invalid Parquet definition levels");
      if(levels_length>bytes.size()-data_pos)return fail("Parquet definition level bounds");
      std::string levels=bytes.substr(data_pos,levels_length);size_t offset=0;data_pos+=levels_length;
      if(!hybrid(levels,offset,1,count,definitions))return fail("invalid Parquet definition level encoding");
    }else definitions.assign(count,1);
    size_t present=0;for(uint32_t d:definitions){if(d>1)return fail("invalid Parquet definition level");present+=d;}
    std::vector<Cell> values;
    if(encoding==0){if(!plain(bytes,data_pos,present,col,values))return fail("invalid Parquet plain values");}
    else if(encoding==2||encoding==8){
      if(data_pos>=bytes.size()&&present)return fail("missing Parquet dictionary indices");
      unsigned width=present?static_cast<unsigned char>(bytes[data_pos++]):0;std::vector<uint32_t> indices;
      if(present&&!hybrid(bytes,data_pos,width,present,indices))return fail("invalid Parquet dictionary indices");
      for(uint32_t index:indices){if(index>=cursor.dictionary.size())return fail("Parquet dictionary index out of bounds");values.push_back(cursor.dictionary[index]);}
    }else if(encoding==3&&schema.field(1).integer==0){
      uint64_t length=0;if(!little(bytes,data_pos,4,length)||length>bytes.size()-data_pos)return fail("invalid Parquet boolean RLE length");
      std::string encoded=bytes.substr(data_pos,length);size_t offset=0;std::vector<uint32_t> booleans;
      if(!hybrid(encoded,offset,1,present,booleans))return fail("invalid Parquet boolean RLE values");
      for(uint32_t boolean:booleans){Cell cell;cell.text=boolean?"true":"false";values.push_back(cell);}
    }else return fail("unsupported Parquet value encoding");
    size_t index=0;for(uint32_t d:definitions){if(d)cursor.values.push_back(values[index++]);else{Cell value;value.null=true;cursor.values.push_back(value);}}
    size_t memory=0;for(const auto &c:cursors_)for(const auto &v:c.values)memory+=sizeof(Cell)+v.text.size();
    if(memory>128*1024*1024)return fail("Parquet page cache exceeds 128 MiB");
    return true;
  }
  int fd_;int64_t size_;CompactValue metadata_;size_t group_=0;int64_t row_=0;bool loaded_=false;
  std::vector<Cursor> cursors_;std::vector<size_t> projection_;bool projection_set_=false;
};

class Reader {
public:
  explicit Reader(Options options) : options_(std::move(options)) {}
  ~Reader() { delete parquet_; if (file_) fclose(file_); }
  Reader(const Reader &) = delete;
  Reader &operator=(const Reader &) = delete;
  std::vector<Column> columns;
  std::string error;
  uint64_t line = 0;
  bool open() {
    int fd=secure_open(options_.path,options_.root);
    if(fd<0){error="file path is outside secure_file_priv or cannot be opened";return false;}
    if(fstat(fd,&identity_)){close(fd);return fail("cannot stat file");}
    file_=fdopen(fd,"rb");if(!file_){close(fd);return fail("cannot open file stream");}
    return true;
  }
  bool unchanged() {
    struct stat st, current;
    if(!file_||fstat(fileno(file_),&st))return fail("cannot stat open file");
#if defined(__APPLE__)
    bool time_same=st.st_mtimespec.tv_sec==identity_.st_mtimespec.tv_sec&&st.st_mtimespec.tv_nsec==identity_.st_mtimespec.tv_nsec;
#else
    bool time_same=st.st_mtim.tv_sec==identity_.st_mtim.tv_sec&&st.st_mtim.tv_nsec==identity_.st_mtim.tv_nsec;
#endif
    if(stat(options_.path.c_str(), &current) || current.st_ino != identity_.st_ino || current.st_dev != identity_.st_dev) return fail("input file replaced during query");
    if(st.st_ino!=identity_.st_ino||st.st_dev!=identity_.st_dev||st.st_size!=identity_.st_size||!time_same)return fail("input file changed during query");
    return true;
  }
  bool rewind() { if(!unchanged())return false;clearerr(file_);std::rewind(file_);line=0;return true; }
  bool infer_schema() {
    if(options_.format=="glob")return infer_glob();
    if(!file_&&!open())return false;
    if(options_.format=="parquet"){
      parquet_=new ParquetReader(fileno(file_),identity_.st_size);
      if(!parquet_->initialize())return fail(parquet_->error);
      columns=parquet_->columns;return check_schema();
    }
    std::vector<Cell> row;
    if(options_.format=="csv") {
      if(!csv(row)){if(error.empty())return fail("CSV file is empty");return false;}
      for(size_t i=0;i<row.size();++i){
        std::string name=options_.header?row[i].text:"column"+std::to_string(i);
        if(name.empty())name="column"+std::to_string(i);
        std::string base=name;unsigned suffix=2;
        auto exists=[&](const std::string &v){for(const auto &c:columns)if(lower(c.name)==lower(v))return true;return false;};
        while(exists(name))name=base+"__"+std::to_string(suffix++);
        columns.push_back({name,Type::Null});
      }
      if(!options_.header)for(size_t i=0;i<row.size();++i){columns[i].type=infer(row[i]);columns[i].nullable=row[i].null;}
      while(csv(row)){
        if(row.size()!=columns.size())return fail("CSV field count differs from header");
        for(size_t i=0;i<row.size();++i){columns[i].type=widen(columns[i].type,infer(row[i]));columns[i].nullable|=row[i].null;}
      }
    } else if(options_.format=="jsonl") {
      std::map<std::string,Cell> object;size_t records=0;
      while(json(object)){
        for(auto &column:columns)if(!object.count(column.name))column.nullable=true;
        for(const auto &entry:object){auto it=std::find_if(columns.begin(),columns.end(),[&](const Column &c){return c.name==entry.first;});
          Type type=infer(entry.second);
          if(entry.second.quoted&&type!=Type::Date&&type!=Type::DateTime)type=Type::String;
          if(it==columns.end()){if(columns.size()>=1024)return fail("too many JSONL fields");columns.push_back({entry.first,type,records>0||entry.second.null});}
          else {it->type=widen(it->type,type);it->nullable|=entry.second.null;}
        }
        ++records;
      }
    } else return fail("unsupported file format");
    if(!error.empty()||columns.empty()||!rewind())return false;
    if(options_.format=="csv"&&options_.header&&!csv(row))return false;
    return check_schema();
  }
  bool next(std::vector<Cell> &row) {
    if(options_.format=="glob") {
      if(glob_pos_==matches_.size())return false;
      row.clear();Cell value;value.text=matches_[glob_pos_++];row.push_back(value);return true;
    }
    if(!unchanged())return false;
    if(parquet_){if(parquet_->next(row))return true;if(!parquet_->error.empty())error=parquet_->error;return false;}
    if(options_.format=="csv"){
      if(!csv(row))return false;
      return row.size()==columns.size()||fail("CSV field count differs from inferred schema");
    }
    std::map<std::string,Cell> object;if(!json(object))return false;
    row.clear();for(const auto &column:columns){auto it=object.find(column.name);if(it==object.end()){Cell c;c.null=true;row.push_back(c);}else row.push_back(it->second);}
    return true;
  }
  void projection(const std::vector<size_t> &ids){if(parquet_)parquet_->projection(ids);}
  bool rescan() {
    if(parquet_)return unchanged()&&parquet_->reset();
    if(options_.format=="glob"){glob_pos_=0;return true;}
    if(!rewind())return false;
    std::vector<Cell> ignored;return options_.format!="csv"||!options_.header||csv(ignored);
  }
  const Options &options() const {return options_;}
private:
  bool check_schema(){return options_.schema.empty()||options_.schema==schema_descriptor(columns)||fail("file schema changed; reprepare the query");}
  bool infer_glob() {
    const size_t slash=options_.path.find_last_of('/');
    std::string directory=slash==std::string::npos?".":slash==0?"/":options_.path.substr(0,slash);
    std::string pattern=slash==std::string::npos?options_.path:options_.path.substr(slash+1);
    if(pattern.empty())return fail("empty glob pattern");
    int fd=secure_open(directory,options_.root,true);
    if(fd<0)return fail("glob directory is outside secure_file_priv or cannot be opened");
    DIR *dir=fdopendir(fd);if(!dir){close(fd);return fail("cannot read glob directory");}
    bool ok=true;
    while(dirent *entry=readdir(dir)) {
      if(fnmatch(pattern.c_str(),entry->d_name,FNM_PERIOD)!=0)continue;
      struct stat st;if(fstatat(fd,entry->d_name,&st,0)||!S_ISREG(st.st_mode))continue;
      std::string path=directory+(directory=="/"?"":"/")+entry->d_name;
      int file=secure_open(path,options_.root);
      if(file<0){ok=false;break;}close(file);
      if(matches_.size()>=10000){ok=false;break;}matches_.push_back(path);
    }
    closedir(dir);
    if(!ok)return fail("glob contains an inaccessible file or exceeds 10000 files");
    std::sort(matches_.begin(),matches_.end());columns.push_back({"file",Type::String});return check_schema();
  }
  bool fail(const std::string &message){error=message+" at record "+std::to_string(line);return false;}
  bool csv(std::vector<Cell> &row) {
    row.clear();Cell cell;bool inside=false,closed=false,any=false;size_t bytes=0;
    while(true){
      int ch=fgetc(file_);
      if(ch==EOF){if(ferror(file_))return fail("file read error");if(inside)return fail("unterminated CSV quoted field");if(!any&&row.empty()&&cell.text.empty())return false;}
      else {any=true;if(++bytes>16*1024*1024)return fail("CSV record exceeds 16 MiB");}
      if(inside){
        if(ch==options_.escape&&options_.escape!=options_.quote){int next=fgetc(file_);if(next==EOF)return fail("unterminated CSV escape");cell.text+=char(next);}
        else if(ch==options_.quote){int next=fgetc(file_);if(next==options_.quote&&options_.escape==options_.quote)cell.text+=char(next);else{inside=false;closed=true;if(next!=EOF)ungetc(next,file_);}}
        else cell.text+=char(ch);
      } else if(ch==EOF||ch==options_.delimiter||ch=='\n'||ch=='\r'){
        if(!valid_utf8(cell.text))return fail("invalid UTF-8 in CSV field");
        if(line==0&&row.empty()&&cell.text.compare(0,3,"\xef\xbb\xbf")==0)cell.text.erase(0,3);
        cell.null=!cell.quoted&&(cell.text.empty()||cell.text==options_.nullstr);row.push_back(cell);cell=Cell();closed=false;
        if(row.size()>1024)return fail("CSV has too many fields");
        if(ch==options_.delimiter)continue;
        if(ch=='\r'){int next=fgetc(file_);if(next!='\n'&&next!=EOF)ungetc(next,file_);}
        ++line;return true;
      } else if(closed)return fail("characters after closing CSV quote");
      else if(ch==options_.quote){if(!cell.text.empty())return fail("quote in unquoted CSV field");inside=true;cell.quoted=true;}
      else cell.text+=char(ch);
    }
  }
  bool json(std::map<std::string,Cell> &object) {
    object.clear();std::string text;int c;
    while((c=fgetc(file_))!=EOF&&c!='\n'){if(text.size()>=16*1024*1024)return fail("JSONL record exceeds 16 MiB");text+=char(c);}
    if(c==EOF&&text.empty())return false;
    ++line;
    if(!valid_utf8(text))return fail("invalid UTF-8 in JSONL record");
    if(!FlatJson(text).parse(object))return fail("invalid flat JSONL object (nested values are unsupported)");
    return true;
  }
  Options options_;FILE *file_=nullptr;struct stat identity_{};
  std::vector<std::string> matches_;size_t glob_pos_=0;ParquetReader *parquet_=nullptr;
};


class CopyWriter {
public:
  explicit CopyWriter(Options options):options_(std::move(options)){}
  ~CopyWriter(){if(file_>=0)close(file_);if(directory_>=0){if(!temporary_.empty())unlinkat(directory_,temporary_.c_str(),0);close(directory_);}}
  std::string error;
  bool open(){
    size_t slash=options_.path.find_last_of('/');
    std::string parent=slash==std::string::npos?".":slash==0?"/":options_.path.substr(0,slash);
    name_=slash==std::string::npos?options_.path:options_.path.substr(slash+1);
    if(name_.empty()||name_=="."||name_=="..")return fail("invalid COPY destination");
    directory_=secure_open(parent,options_.root,true);
    if(directory_<0)return fail("COPY destination is outside secure_file_priv");
    struct stat st;if(fstatat(directory_,name_.c_str(),&st,AT_SYMLINK_NOFOLLOW)==0||errno!=ENOENT)return fail("COPY destination already exists or is inaccessible");
    struct timespec now;clock_gettime(CLOCK_MONOTONIC,&now);
    for(int attempt=0;attempt<100;++attempt){
      temporary_=".seekdb-copy-"+std::to_string(getpid())+"-"+std::to_string(now.tv_nsec)+"-"+std::to_string(attempt);
      file_=openat(directory_,temporary_.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
      if(file_>=0)return true;if(errno!=EEXIST)break;
    }
    temporary_.clear();return fail("cannot create COPY temporary file");
  }
  bool row(const std::vector<Cell> &cells){
    std::string line;
    for(size_t i=0;i<cells.size();++i){
      if(i)line+=options_.delimiter;
      const auto &cell=cells[i];
      if(cell.null){line+=options_.nullstr;continue;}
      bool quote=cell.text.empty()||cell.text==options_.nullstr||cell.text.find_first_of(std::string()+options_.delimiter+options_.quote+'\r'+'\n')!=std::string::npos;
      if(quote)line+=options_.quote;
      for(char c:cell.text){if(quote&&(c==options_.quote||(c==options_.escape&&options_.escape!=options_.quote)))line+=options_.escape;line+=c;}
      if(quote)line+=options_.quote;
      if(line.size()>64*1024*1024)return fail("COPY output record exceeds 64 MiB");
    }
    line+='\n';size_t pos=0;
    while(pos<line.size()){ssize_t n=write(file_,line.data()+pos,line.size()-pos);if(n<0&&errno==EINTR)continue;if(n<=0)return fail("COPY write failed");pos+=n;}
    return true;
  }
  bool commit(){
    if(file_<0||fsync(file_))return fail("cannot flush COPY output");
    if(close(file_)){file_=-1;return fail("cannot close COPY output");}file_=-1;
    // linkat publishes without replacing an existing destination, including symlinks.
    if(linkat(directory_,temporary_.c_str(),directory_,name_.c_str(),0))return fail("COPY destination already exists or cannot be published");
    unlinkat(directory_,temporary_.c_str(),0);temporary_.clear();return true;
  }
private:
  bool fail(const char *text){error=text;return false;}
  Options options_;int directory_=-1,file_=-1;std::string name_,temporary_;
};

}}} // namespace oceanbase::sql::file_sql
