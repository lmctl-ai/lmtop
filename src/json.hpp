#pragma once
// Minimal JSON value, parser and serializer for the lmtop stdio protocol.
// One-line serialization; numbers are int64 when integral, double otherwise.
// NaN/Infinity serialize as null (callers convert missing values to -1 first,
// following the lmdebug convention).

#include <cstdint>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace lmtop {

struct Json {
  using Array = std::vector<Json>;
  using Object = std::vector<std::pair<std::string, Json>>;
  std::variant<std::nullptr_t, bool, int64_t, double, std::string, Array, Object> v = nullptr;

  Json() = default;
  Json(std::nullptr_t) {}
  Json(bool b) : v(b) {}
  Json(int i) : v(static_cast<int64_t>(i)) {}
  Json(int64_t i) : v(i) {}
  Json(uint64_t i) : v(static_cast<int64_t>(i)) {}
  Json(double d) : v(d) {}
  Json(const char *s) : v(std::string(s)) {}
  Json(std::string s) : v(std::move(s)) {}
  Json(Array a) : v(std::move(a)) {}
  Json(Object o) : v(std::move(o)) {}

  static Json obj(std::initializer_list<std::pair<std::string, Json>> fields) {
    return Json(Object(fields));
  }

  bool is_null() const { return std::holds_alternative<std::nullptr_t>(v); }
  bool is_bool() const { return std::holds_alternative<bool>(v); }
  bool is_int() const { return std::holds_alternative<int64_t>(v); }
  bool is_double() const { return std::holds_alternative<double>(v); }
  bool is_number() const { return is_int() || is_double(); }
  bool is_string() const { return std::holds_alternative<std::string>(v); }
  bool is_array() const { return std::holds_alternative<Array>(v); }
  bool is_object() const { return std::holds_alternative<Object>(v); }

  int64_t as_int(int64_t fallback = 0) const {
    if (is_int()) return std::get<int64_t>(v);
    if (is_double()) return static_cast<int64_t>(std::get<double>(v));
    return fallback;
  }
  double as_double(double fallback = 0.0) const {
    if (is_double()) return std::get<double>(v);
    if (is_int()) return static_cast<double>(std::get<int64_t>(v));
    return fallback;
  }
  const std::string &as_string() const {
    static const std::string empty;
    return is_string() ? std::get<std::string>(v) : empty;
  }
  const Array &as_array() const {
    static const Array empty;
    return is_array() ? std::get<Array>(v) : empty;
  }
  const Object &as_object() const {
    static const Object empty;
    return is_object() ? std::get<Object>(v) : empty;
  }

  // Object lookup; nullptr when absent or not an object.
  const Json *find(const std::string &key) const {
    if (!is_object()) return nullptr;
    for (const auto &[k, val] : std::get<Object>(v))
      if (k == key) return &val;
    return nullptr;
  }

  Json &set(const std::string &key, Json value) {
    Object &o = std::get<Object>(v);
    for (auto &[k, val] : o)
      if (k == key) { val = std::move(value); return *this; }
    o.emplace_back(key, std::move(value));
    return *this;
  }

  Json &push(Json value) {
    std::get<Array>(v).push_back(std::move(value));
    return *this;
  }
};

// Serializes to a single line (no trailing newline).
std::string dump(const Json &value);
// Parses a complete JSON document. Returns false and fills `error`.
bool parse(const std::string &text, Json &out, std::string &error);

} // namespace lmtop
