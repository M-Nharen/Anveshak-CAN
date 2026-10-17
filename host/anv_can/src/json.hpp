// Just enough JSON to read schema/generated/schema.json. Copied from the gateway (host/gateway/src/
// json_mini.hpp) so this library does not depend on the old 11-bit gateway, which goes away.
// Private to anv_can (not installed).
#pragma once

#include <cstdlib>
#include <map>
#include <string>
#include <vector>

namespace anv::json {

struct Value {
  enum Type { Null, Bool, Number, String, Array, Object } type = Null;
  bool b = false;
  double num = 0;
  std::string str;
  std::vector<Value> arr;
  std::map<std::string, Value> obj;
  std::vector<std::string> keys;  // object keys in file order (enum labels keep their order)

  const Value* get(const std::string& key) const {
    auto it = obj.find(key);
    return it == obj.end() ? nullptr : &it->second;
  }
};

class Parser {
 public:
  explicit Parser(const std::string& s) : s_(s) {}
  // False on any syntax error or trailing garbage; depth is limited so hostile input is safe.
  bool parse(Value& out) {
    skip();
    if (!value(out, 0)) return false;
    skip();
    return pos_ == s_.size();
  }

 private:
  void skip() {
    while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\r' || s_[pos_] == '\n')) ++pos_;
  }
  bool lit(const char* w) {
    size_t n = std::char_traits<char>::length(w);
    if (s_.compare(pos_, n, w) != 0) return false;
    pos_ += n;
    return true;
  }
  bool string(std::string& out) {
    if (pos_ >= s_.size() || s_[pos_] != '"') return false;
    ++pos_;
    while (pos_ < s_.size()) {
      char c = s_[pos_++];
      if (c == '"') return true;
      if (c == '\\') {
        if (pos_ >= s_.size()) return false;
        char e = s_[pos_++];
        switch (e) {
          case '"': out.push_back('"'); break;
          case '\\': out.push_back('\\'); break;
          case '/': out.push_back('/'); break;
          case 'n': out.push_back('\n'); break;
          case 't': out.push_back('\t'); break;
          case 'r': out.push_back('\r'); break;
          case 'b': out.push_back('\b'); break;
          case 'f': out.push_back('\f'); break;
          case 'u': {  // keep ASCII only; names in the schema are ASCII
            if (pos_ + 4 > s_.size()) return false;
            long cp = std::strtol(s_.substr(pos_, 4).c_str(), nullptr, 16);
            pos_ += 4;
            out.push_back(cp < 128 ? static_cast<char>(cp) : '?');
            break;
          }
          default: return false;
        }
      } else {
        out.push_back(c);
      }
    }
    return false;
  }
  bool value(Value& v, int depth) {
    if (depth > 32 || pos_ >= s_.size()) return false;
    char c = s_[pos_];
    if (c == '{') {
      v.type = Value::Object;
      ++pos_;
      skip();
      if (pos_ < s_.size() && s_[pos_] == '}') { ++pos_; return true; }
      while (true) {
        skip();
        std::string key;
        if (!string(key)) return false;
        skip();
        if (pos_ >= s_.size() || s_[pos_++] != ':') return false;
        skip();
        Value child;
        if (!value(child, depth + 1)) return false;
        if (v.obj.find(key) == v.obj.end()) v.keys.push_back(key);
        v.obj[key] = std::move(child);
        skip();
        if (pos_ >= s_.size()) return false;
        if (s_[pos_] == ',') { ++pos_; continue; }
        if (s_[pos_] == '}') { ++pos_; return true; }
        return false;
      }
    }
    if (c == '[') {
      v.type = Value::Array;
      ++pos_;
      skip();
      if (pos_ < s_.size() && s_[pos_] == ']') { ++pos_; return true; }
      while (true) {
        skip();
        Value child;
        if (!value(child, depth + 1)) return false;
        v.arr.push_back(std::move(child));
        skip();
        if (pos_ >= s_.size()) return false;
        if (s_[pos_] == ',') { ++pos_; continue; }
        if (s_[pos_] == ']') { ++pos_; return true; }
        return false;
      }
    }
    if (c == '"') { v.type = Value::String; return string(v.str); }
    if (lit("true")) { v.type = Value::Bool; v.b = true; return true; }
    if (lit("false")) { v.type = Value::Bool; v.b = false; return true; }
    if (lit("null")) { v.type = Value::Null; return true; }
    const char* start = s_.c_str() + pos_;
    char* end = nullptr;
    double d = std::strtod(start, &end);
    if (end == start) return false;
    pos_ += static_cast<size_t>(end - start);
    v.type = Value::Number;
    v.num = d;
    return true;
  }

  const std::string& s_;
  size_t pos_ = 0;
};

}  // namespace anv::json
