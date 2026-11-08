#include "json.hpp"

#include <charconv>
#include <cmath>
#include <cstdio>
#include <system_error>

namespace lmtop {
namespace {

void dump_string(const std::string &s, std::string &out) {
  out += '"';
  char hex[8];
  for (unsigned char c : s) {
    switch (c) {
    case '"': out += "\\\""; break;
    case '\\': out += "\\\\"; break;
    case '\b': out += "\\b"; break;
    case '\f': out += "\\f"; break;
    case '\n': out += "\\n"; break;
    case '\r': out += "\\r"; break;
    case '\t': out += "\\t"; break;
    default:
      if (c < 0x20) {
        std::snprintf(hex, sizeof hex, "\\u%04x", c);
        out += hex;
      } else {
        out += static_cast<char>(c);
      }
    }
  }
  out += '"';
}

void dump_value(const Json &value, std::string &out) {
  if (value.is_null()) { out += "null"; return; }
  if (value.is_bool()) { out += std::get<bool>(value.v) ? "true" : "false"; return; }
  if (value.is_int()) {
    char buf[24];
    const auto res = std::to_chars(buf, buf + sizeof buf, std::get<int64_t>(value.v));
    out.append(buf, res.ptr);
    return;
  }
  if (value.is_double()) {
    const double d = std::get<double>(value.v);
    if (!std::isfinite(d)) { out += "null"; return; }
    char buf[40];
    const auto res = std::to_chars(buf, buf + sizeof buf, d, std::chars_format::general);
    out.append(buf, res.ptr);
    return;
  }
  if (value.is_string()) { dump_string(std::get<std::string>(value.v), out); return; }
  if (value.is_array()) {
    out += '[';
    bool first = true;
    for (const auto &item : std::get<Json::Array>(value.v)) {
      if (!first) out += ',';
      first = false;
      dump_value(item, out);
    }
    out += ']';
    return;
  }
  out += '{';
  bool first = true;
  for (const auto &[key, val] : std::get<Json::Object>(value.v)) {
    if (!first) out += ',';
    first = false;
    dump_string(key, out);
    out += ':';
    dump_value(val, out);
  }
  out += '}';
}

struct Parser {
  const char *p;
  const char *end;
  std::string error;
  int depth = 0;

  bool fail(const std::string &message) {
    if (error.empty()) error = message;
    return false;
  }

  void skip_ws() {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p;
  }

  bool literal(const char *word, Json value, Json &out) {
    const size_t n = std::char_traits<char>::length(word);
    if (static_cast<size_t>(end - p) < n || std::string(p, n) != word)
      return fail("invalid literal");
    p += n;
    out = std::move(value);
    return true;
  }

  static void encode_utf8(uint32_t cp, std::string &out) {
    if (cp < 0x80) out += static_cast<char>(cp);
    else if (cp < 0x800) {
      out += static_cast<char>(0xC0 | (cp >> 6));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
      out += static_cast<char>(0xE0 | (cp >> 12));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
      out += static_cast<char>(0xF0 | (cp >> 18));
      out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    }
  }

  bool hex4(uint32_t &cp) {
    if (end - p < 4) return fail("truncated \\u escape");
    cp = 0;
    for (int i = 0; i < 4; ++i) {
      const char c = *p++;
      cp <<= 4;
      if (c >= '0' && c <= '9') cp |= static_cast<uint32_t>(c - '0');
      else if (c >= 'a' && c <= 'f') cp |= static_cast<uint32_t>(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') cp |= static_cast<uint32_t>(c - 'A' + 10);
      else return fail("bad \\u escape");
    }
    return true;
  }

  bool parse_string(std::string &out) {
    if (p >= end || *p != '"') return fail("expected string");
    ++p;
    out.clear();
    while (p < end) {
      const unsigned char c = static_cast<unsigned char>(*p++);
      if (c == '"') return true;
      if (c == '\\') {
        if (p >= end) return fail("truncated escape");
        const char esc = *p++;
        switch (esc) {
        case '"': out += '"'; break;
        case '\\': out += '\\'; break;
        case '/': out += '/'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        case 'u': {
          uint32_t cp = 0;
          if (!hex4(cp)) return false;
          if (cp >= 0xD800 && cp <= 0xDBFF) {
            // High surrogate: require a following \uDC00..\uDFFF pair.
            if (end - p < 2 || p[0] != '\\' || p[1] != 'u')
              return fail("lone high surrogate");
            p += 2;
            uint32_t low = 0;
            if (!hex4(low)) return false;
            if (low < 0xDC00 || low > 0xDFFF) return fail("lone high surrogate");
            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
          } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            return fail("lone low surrogate");
          }
          encode_utf8(cp, out);
          break;
        }
        default: return fail("unknown escape");
        }
        continue;
      }
      if (c < 0x20) return fail("control character in string");
      out += static_cast<char>(c);
    }
    return fail("unterminated string");
  }

  bool parse_number(Json &out) {
    const char *start = p;
    if (p < end && *p == '-') ++p;
    bool integral = true;
    while (p < end && *p >= '0' && *p <= '9') ++p;
    if (p < end && *p == '.') {
      integral = false;
      ++p;
      while (p < end && *p >= '0' && *p <= '9') ++p;
    }
    if (p < end && (*p == 'e' || *p == 'E')) {
      integral = false;
      ++p;
      if (p < end && (*p == '+' || *p == '-')) ++p;
      while (p < end && *p >= '0' && *p <= '9') ++p;
    }
    if (p == start || (p == start + 1 && *start == '-')) return fail("invalid number");
    const std::string token(start, p);
    if (integral) {
      int64_t value = 0;
      const auto res = std::from_chars(token.data(), token.data() + token.size(), value);
      if (res.ec == std::errc() && res.ptr == token.data() + token.size()) {
        out = Json(value);
        return true;
      }
      // Fall through to double on overflow.
    }
    try {
      size_t used = 0;
      const double value = std::stod(token, &used);
      if (used != token.size() || !std::isfinite(value)) return fail("invalid number");
      out = Json(value);
      return true;
    } catch (...) {
      return fail("invalid number");
    }
  }

  bool parse_array(Json &out) {
    if (++depth > 200) return fail("nesting too deep");
    ++p; // '['
    Json::Array items;
    skip_ws();
    if (p < end && *p == ']') { ++p; out = Json(std::move(items)); --depth; return true; }
    while (true) {
      Json item;
      if (!parse_value(item)) return false;
      items.push_back(std::move(item));
      skip_ws();
      if (p >= end) return fail("unterminated array");
      if (*p == ',') { ++p; continue; }
      if (*p == ']') { ++p; break; }
      return fail("expected ',' or ']'");
    }
    out = Json(std::move(items));
    --depth;
    return true;
  }

  bool parse_object(Json &out) {
    if (++depth > 200) return fail("nesting too deep");
    ++p; // '{'
    Json::Object fields;
    skip_ws();
    if (p < end && *p == '}') { ++p; out = Json(std::move(fields)); --depth; return true; }
    while (true) {
      skip_ws();
      std::string key;
      if (!parse_string(key)) return false;
      skip_ws();
      if (p >= end || *p != ':') return fail("expected ':'");
      ++p;
      Json value;
      if (!parse_value(value)) return false;
      fields.emplace_back(std::move(key), std::move(value));
      skip_ws();
      if (p >= end) return fail("unterminated object");
      if (*p == ',') { ++p; continue; }
      if (*p == '}') { ++p; break; }
      return fail("expected ',' or '}'");
    }
    out = Json(std::move(fields));
    --depth;
    return true;
  }

  bool parse_value(Json &out) {
    skip_ws();
    if (p >= end) return fail("unexpected end of input");
    switch (*p) {
    case 'n': return literal("null", Json(nullptr), out);
    case 't': return literal("true", Json(true), out);
    case 'f': return literal("false", Json(false), out);
    case '"': {
      std::string s;
      if (!parse_string(s)) return false;
      out = Json(std::move(s));
      return true;
    }
    case '[': return parse_array(out);
    case '{': return parse_object(out);
    default: return parse_number(out);
    }
  }
};

} // namespace

std::string dump(const Json &value) {
  std::string out;
  dump_value(value, out);
  return out;
}

bool parse(const std::string &text, Json &out, std::string &error) {
  Parser parser{text.data(), text.data() + text.size(), {}, 0};
  if (!parser.parse_value(out)) {
    error = parser.error;
    return false;
  }
  parser.skip_ws();
  if (parser.p != parser.end) {
    error = "trailing data after JSON document";
    return false;
  }
  return true;
}

} // namespace lmtop
