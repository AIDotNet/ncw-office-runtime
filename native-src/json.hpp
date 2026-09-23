// 最小 JSON 实现 —— 只服务于 ncw-office-helper 与宿主之间的控制帧。
//
// 需求:helper 要解析宿主发来的请求(操作批次)和 LibreOfficeKit 回调里的 JSON 负载,
// 同时要产出回执。引入第三方 JSON 库会让原生构建多一个要随三平台锁版本、审许可证的
// 依赖;控制帧的形状很小,这里写一个够用、有上限的实现。
//
// 不变式:
// - 解析失败抛 std::runtime_error,调用方把它变成 `invalid_operation` 回执,不崩进程。
// - 嵌套深度有上限:宿主是可信的,但 LOK 回调负载来自引擎处理的文档内容。
#pragma once

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace ncw {

struct Json {
  enum class Type { Null, Bool, Number, String, Array, Object };
  Type type = Type::Null;
  bool boolean = false;
  double number = 0;
  std::string string;
  std::vector<Json> array;
  std::vector<std::pair<std::string, Json>> object;

  bool isNull() const { return type == Type::Null; }
  bool isString() const { return type == Type::String; }
  bool isNumber() const { return type == Type::Number; }
  bool isBool() const { return type == Type::Bool; }
  bool isArray() const { return type == Type::Array; }
  bool isObject() const { return type == Type::Object; }

  const Json* get(const std::string& key) const {
    if (type != Type::Object) return nullptr;
    for (const auto& entry : object)
      if (entry.first == key) return &entry.second;
    return nullptr;
  }
  std::string str(const std::string& key, const std::string& fallback = "") const {
    const Json* value = get(key);
    return value != nullptr && value->isString() ? value->string : fallback;
  }
};

class JsonParser {
 public:
  explicit JsonParser(const std::string& text) : s_(text) {}

  Json parse() {
    Json value = parseValue(0);
    skipSpace();
    if (pos_ != s_.size()) fail("trailing characters");
    return value;
  }

 private:
  static constexpr int kMaxDepth = 64;
  const std::string& s_;
  size_t pos_ = 0;

  [[noreturn]] void fail(const char* what) const {
    throw std::runtime_error(std::string("invalid JSON: ") + what);
  }

  void skipSpace() {
    while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\n' || s_[pos_] == '\r' || s_[pos_] == '\t')) ++pos_;
  }

  bool consume(const char* literal) {
    size_t n = std::strlen(literal);
    if (s_.compare(pos_, n, literal) == 0) { pos_ += n; return true; }
    return false;
  }

  Json parseValue(int depth) {
    if (depth > kMaxDepth) fail("nesting too deep");
    skipSpace();
    if (pos_ >= s_.size()) fail("unexpected end");
    Json out;
    char c = s_[pos_];
    if (c == '{') {
      out.type = Json::Type::Object;
      ++pos_;
      skipSpace();
      if (pos_ < s_.size() && s_[pos_] == '}') { ++pos_; return out; }
      for (;;) {
        skipSpace();
        if (pos_ >= s_.size() || s_[pos_] != '"') fail("expected key");
        std::string key = parseString();
        skipSpace();
        if (pos_ >= s_.size() || s_[pos_] != ':') fail("expected ':'");
        ++pos_;
        out.object.emplace_back(std::move(key), parseValue(depth + 1));
        skipSpace();
        if (pos_ < s_.size() && s_[pos_] == ',') { ++pos_; continue; }
        if (pos_ < s_.size() && s_[pos_] == '}') { ++pos_; return out; }
        fail("expected ',' or '}'");
      }
    }
    if (c == '[') {
      out.type = Json::Type::Array;
      ++pos_;
      skipSpace();
      if (pos_ < s_.size() && s_[pos_] == ']') { ++pos_; return out; }
      for (;;) {
        out.array.push_back(parseValue(depth + 1));
        skipSpace();
        if (pos_ < s_.size() && s_[pos_] == ',') { ++pos_; continue; }
        if (pos_ < s_.size() && s_[pos_] == ']') { ++pos_; return out; }
        fail("expected ',' or ']'");
      }
    }
    if (c == '"') { out.type = Json::Type::String; out.string = parseString(); return out; }
    if (consume("true")) { out.type = Json::Type::Bool; out.boolean = true; return out; }
    if (consume("false")) { out.type = Json::Type::Bool; out.boolean = false; return out; }
    if (consume("null")) return out;
    if (c == '-' || (c >= '0' && c <= '9')) {
      size_t start = pos_;
      if (s_[pos_] == '-') ++pos_;
      while (pos_ < s_.size() && std::strchr("0123456789.eE+-", s_[pos_]) != nullptr) ++pos_;
      std::string token = s_.substr(start, pos_ - start);
      char* end = nullptr;
      double value = std::strtod(token.c_str(), &end);
      if (end == token.c_str() || *end != '\0' || !std::isfinite(value)) fail("bad number");
      out.type = Json::Type::Number;
      out.number = value;
      return out;
    }
    fail("unexpected character");
  }

  static void appendUtf8(std::string& out, unsigned code) {
    if (code < 0x80) { out += static_cast<char>(code); }
    else if (code < 0x800) { out += static_cast<char>(0xC0 | (code >> 6)); out += static_cast<char>(0x80 | (code & 0x3F)); }
    else if (code < 0x10000) {
      out += static_cast<char>(0xE0 | (code >> 12)); out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (code & 0x3F));
    } else {
      out += static_cast<char>(0xF0 | (code >> 18)); out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
      out += static_cast<char>(0x80 | ((code >> 6) & 0x3F)); out += static_cast<char>(0x80 | (code & 0x3F));
    }
  }

  unsigned parseHex4() {
    if (pos_ + 4 > s_.size()) fail("short \\u escape");
    unsigned value = 0;
    for (int i = 0; i < 4; ++i) {
      char h = s_[pos_++];
      value <<= 4;
      if (h >= '0' && h <= '9') value |= static_cast<unsigned>(h - '0');
      else if (h >= 'a' && h <= 'f') value |= static_cast<unsigned>(h - 'a' + 10);
      else if (h >= 'A' && h <= 'F') value |= static_cast<unsigned>(h - 'A' + 10);
      else fail("bad \\u escape");
    }
    return value;
  }

  std::string parseString() {
    ++pos_;  // opening quote
    std::string out;
    while (pos_ < s_.size()) {
      char c = s_[pos_++];
      if (c == '"') return out;
      if (c != '\\') { out += c; continue; }
      if (pos_ >= s_.size()) break;
      char e = s_[pos_++];
      switch (e) {
        case '"': out += '"'; break;
        case '\\': out += '\\'; break;
        case '/': out += '/'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        case 'u': {
          unsigned code = parseHex4();
          if (code >= 0xD800 && code <= 0xDBFF && pos_ + 6 <= s_.size() && s_[pos_] == '\\' && s_[pos_ + 1] == 'u') {
            pos_ += 2;
            unsigned low = parseHex4();
            if (low < 0xDC00 || low > 0xDFFF) fail("bad surrogate pair");
            code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
          }
          appendUtf8(out, code);
          break;
        }
        default: fail("bad escape");
      }
    }
    fail("unterminated string");
  }
};

inline Json parseJson(const std::string& text) { return JsonParser(text).parse(); }

/** JSON 字符串字面量(带引号)。控制字符一律 \u 转义,UTF-8 原样透传。 */
inline std::string quote(const std::string& value) {
  std::string out = "\"";
  for (unsigned char c : value) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof buf, "\\u%04x", c);
          out += buf;
        } else {
          out += static_cast<char>(c);
        }
    }
  }
  out += '"';
  return out;
}

}  // namespace ncw
