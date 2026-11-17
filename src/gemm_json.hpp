#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace axissim::gemm_json {

struct Number {
  std::string lexeme;
};
struct Value;
using Array = std::vector<Value>;
using Object = std::map<std::string, Value>;
struct Value {
  using Storage =
      std::variant<std::nullptr_t, bool, Number, std::string, Array, Object>;
  Storage storage = nullptr;
  Value() = default;
  Value(std::nullptr_t) : storage(nullptr) {}
  Value(bool value) : storage(value) {}
  Value(Number value) : storage(std::move(value)) {}
  Value(std::uint64_t value);
  Value(std::string value) : storage(std::move(value)) {}
  Value(const char *value) : storage(std::string(value)) {}
  Value(Array value) : storage(std::move(value)) {}
  Value(Object value) : storage(std::move(value)) {}
  [[nodiscard]] bool is_bool() const;
  [[nodiscard]] bool is_number() const;
  [[nodiscard]] bool is_string() const;
  [[nodiscard]] bool is_array() const;
  [[nodiscard]] bool is_object() const;
  [[nodiscard]] bool is_null() const;
  [[nodiscard]] bool is_unsigned_integer() const;
  [[nodiscard]] std::uint64_t unsigned_integer() const;
  [[nodiscard]] const std::string &string() const;
  [[nodiscard]] const Array &array() const;
  [[nodiscard]] const Object &object() const;
};

class Error : public std::runtime_error {
public:
  Error(std::string code, std::string path);
  std::string code;
  std::string path;
};

[[nodiscard]] Value parse(std::string_view input);
[[nodiscard]] std::string canonical(const Value &value);
[[nodiscard]] std::string pointer_child(std::string_view parent,
                                        std::string_view key);
[[nodiscard]] std::string quote(std::string_view ascii);

} // namespace axissim::gemm_json
