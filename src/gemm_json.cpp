#include "gemm_json.hpp"

#include <algorithm>
#include <charconv>

namespace axissim::gemm_json {
namespace {

[[nodiscard]] bool valid_utf8(std::string_view text) {
  for (std::size_t i = 0; i < text.size();) {
    const auto first = static_cast<unsigned char>(text[i]);
    if (first <= 0x7f) {
      ++i;
      continue;
    }
    std::size_t count = 0;
    std::uint32_t code = 0;
    if ((first & 0xe0) == 0xc0) {
      count = 2;
      code = first & 0x1f;
    } else if ((first & 0xf0) == 0xe0) {
      count = 3;
      code = first & 0x0f;
    } else if ((first & 0xf8) == 0xf0) {
      count = 4;
      code = first & 0x07;
    } else
      return false;
    if (i + count > text.size())
      return false;
    for (std::size_t j = 1; j < count; ++j) {
      const auto next = static_cast<unsigned char>(text[i + j]);
      if ((next & 0xc0) != 0x80)
        return false;
      code = (code << 6) | (next & 0x3f);
    }
    if ((count == 2 && code < 0x80) || (count == 3 && code < 0x800) ||
        (count == 4 && code < 0x10000) || code > 0x10ffff ||
        (code >= 0xd800 && code <= 0xdfff))
      return false;
    i += count;
  }
  return true;
}

void append_utf8(std::string &out, std::uint32_t code) {
  if (code <= 0x7f)
    out.push_back(static_cast<char>(code));
  else if (code <= 0x7ff) {
    out.push_back(static_cast<char>(0xc0 | (code >> 6)));
    out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
  } else if (code <= 0xffff) {
    out.push_back(static_cast<char>(0xe0 | (code >> 12)));
    out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
    out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
  } else {
    out.push_back(static_cast<char>(0xf0 | (code >> 18)));
    out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3f)));
    out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
    out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
  }
}

class Parser {
public:
  explicit Parser(std::string_view input) : input_(input) {
    if (!valid_utf8(input))
      fail();
  }
  [[nodiscard]] Value run() {
    skip_space();
    auto value = parse_value("", 0);
    skip_space();
    if (position_ != input_.size())
      fail();
    if (!duplicate_paths_.empty()) {
      std::sort(duplicate_paths_.begin(), duplicate_paths_.end());
      throw Error("duplicate_key", duplicate_paths_.front());
    }
    return value;
  }

private:
  [[noreturn]] void fail(std::string path = "") const {
    throw Error("invalid_json", std::move(path));
  }
  void skip_space() {
    while (position_ < input_.size() &&
           (input_[position_] == ' ' || input_[position_] == '\n' ||
            input_[position_] == '\r' || input_[position_] == '\t'))
      ++position_;
  }
  [[nodiscard]] bool consume(char value) {
    if (position_ < input_.size() && input_[position_] == value) {
      ++position_;
      return true;
    }
    return false;
  }
  [[nodiscard]] std::uint32_t hex4() {
    if (input_.size() - position_ < 4)
      fail();
    std::uint32_t result = 0;
    for (int i = 0; i < 4; ++i) {
      const char c = input_[position_++];
      result <<= 4;
      if (c >= '0' && c <= '9')
        result |= static_cast<std::uint32_t>(c - '0');
      else if (c >= 'a' && c <= 'f')
        result |= static_cast<std::uint32_t>(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F')
        result |= static_cast<std::uint32_t>(c - 'A' + 10);
      else
        fail();
    }
    return result;
  }
  [[nodiscard]] std::string parse_string(std::string_view path) {
    if (!consume('"'))
      fail(std::string(path));
    std::string result;
    while (position_ < input_.size()) {
      const unsigned char c = static_cast<unsigned char>(input_[position_++]);
      if (c == '"')
        return result;
      if (c < 0x20)
        fail(std::string(path));
      if (c != '\\') {
        result.push_back(static_cast<char>(c));
        continue;
      }
      if (position_ == input_.size())
        fail(std::string(path));
      const char escaped = input_[position_++];
      switch (escaped) {
      case '"':
        result.push_back('"');
        break;
      case '\\':
        result.push_back('\\');
        break;
      case '/':
        result.push_back('/');
        break;
      case 'b':
        result.push_back('\b');
        break;
      case 'f':
        result.push_back('\f');
        break;
      case 'n':
        result.push_back('\n');
        break;
      case 'r':
        result.push_back('\r');
        break;
      case 't':
        result.push_back('\t');
        break;
      case 'u': {
        std::uint32_t code = hex4();
        if (code >= 0xd800 && code <= 0xdbff) {
          if (input_.size() - position_ < 6 || input_[position_] != '\\' ||
              input_[position_ + 1] != 'u')
            fail(std::string(path));
          position_ += 2;
          const auto low = hex4();
          if (low < 0xdc00 || low > 0xdfff)
            fail(std::string(path));
          code = 0x10000 + ((code - 0xd800) << 10) + (low - 0xdc00);
        } else if (code >= 0xdc00 && code <= 0xdfff)
          fail(std::string(path));
        append_utf8(result, code);
        break;
      }
      default:
        fail(std::string(path));
      }
    }
    fail(std::string(path));
  }
  [[nodiscard]] Value parse_number(std::string_view path) {
    const std::size_t start = position_;
    (void)consume('-');
    if (position_ == input_.size())
      fail(std::string(path));
    if (input_[position_] == '0') {
      ++position_;
      if (position_ < input_.size() && input_[position_] >= '0' &&
          input_[position_] <= '9')
        fail(std::string(path));
    } else {
      if (input_[position_] < '1' || input_[position_] > '9')
        fail(std::string(path));
      while (position_ < input_.size() && input_[position_] >= '0' &&
             input_[position_] <= '9')
        ++position_;
    }
    if (consume('.')) {
      if (position_ == input_.size() || input_[position_] < '0' ||
          input_[position_] > '9')
        fail(std::string(path));
      while (position_ < input_.size() && input_[position_] >= '0' &&
             input_[position_] <= '9')
        ++position_;
    }
    if (position_ < input_.size() &&
        (input_[position_] == 'e' || input_[position_] == 'E')) {
      ++position_;
      if (position_ < input_.size() &&
          (input_[position_] == '+' || input_[position_] == '-'))
        ++position_;
      if (position_ == input_.size() || input_[position_] < '0' ||
          input_[position_] > '9')
        fail(std::string(path));
      while (position_ < input_.size() && input_[position_] >= '0' &&
             input_[position_] <= '9')
        ++position_;
    }
    return Value(Number{std::string(input_.substr(start, position_ - start))});
  }
  [[nodiscard]] Value parse_value(std::string path, std::size_t depth) {
    if (depth > 64 || position_ == input_.size())
      fail(path);
    skip_space();
    if (position_ == input_.size())
      fail(path);
    const char c = input_[position_];
    if (c == '"')
      return Value(parse_string(path));
    if (c == '{') {
      ++position_;
      skip_space();
      Object object;
      if (consume('}'))
        return Value(std::move(object));
      while (true) {
        skip_space();
        if (position_ == input_.size() || input_[position_] != '"')
          fail(path);
        const std::string key = parse_string(path);
        const std::string child_path = pointer_child(path, key);
        skip_space();
        if (!consume(':'))
          fail(child_path);
        auto child = parse_value(child_path, depth + 1);
        if (!object.emplace(key, std::move(child)).second)
          duplicate_paths_.push_back(child_path);
        skip_space();
        if (consume('}'))
          break;
        if (!consume(','))
          fail(path);
      }
      return Value(std::move(object));
    }
    if (c == '[') {
      ++position_;
      skip_space();
      Array array;
      if (consume(']'))
        return Value(std::move(array));
      while (true) {
        const auto child_path =
            pointer_child(path, std::to_string(array.size()));
        array.push_back(parse_value(child_path, depth + 1));
        skip_space();
        if (consume(']'))
          break;
        if (!consume(','))
          fail(path);
      }
      return Value(std::move(array));
    }
    if (c == 't' && input_.substr(position_, 4) == "true") {
      position_ += 4;
      return Value(true);
    }
    if (c == 'f' && input_.substr(position_, 5) == "false") {
      position_ += 5;
      return Value(false);
    }
    if (c == 'n' && input_.substr(position_, 4) == "null") {
      position_ += 4;
      return Value(nullptr);
    }
    if (c == '-' || (c >= '0' && c <= '9'))
      return parse_number(path);
    fail(path);
  }

  std::string_view input_;
  std::size_t position_ = 0;
  std::vector<std::string> duplicate_paths_;
};

void append_canonical(std::string &out, const Value &value) {
  if (value.is_null())
    throw Error("invalid_value", "");
  if (value.is_bool()) {
    out += std::get<bool>(value.storage) ? "true" : "false";
    return;
  }
  if (value.is_number()) {
    const auto &lexeme = std::get<Number>(value.storage).lexeme;
    std::uint64_t parsed = 0;
    const auto [end, error] =
        std::from_chars(lexeme.data(), lexeme.data() + lexeme.size(), parsed);
    if (lexeme.empty() || lexeme.front() == '-' || error != std::errc{} ||
        end != lexeme.data() + lexeme.size())
      throw Error("invalid_value", "");
    out += std::to_string(parsed);
    return;
  }
  if (value.is_string()) {
    out += quote(value.string());
    return;
  }
  if (value.is_array()) {
    out.push_back('[');
    bool first = true;
    for (const auto &child : value.array()) {
      if (!first)
        out.push_back(',');
      first = false;
      append_canonical(out, child);
    }
    out.push_back(']');
    return;
  }
  out.push_back('{');
  bool first = true;
  for (const auto &[key, child] : value.object()) {
    if (!first)
      out.push_back(',');
    first = false;
    out += quote(key);
    out.push_back(':');
    append_canonical(out, child);
  }
  out.push_back('}');
}

} // namespace

Value::Value(std::uint64_t value) : storage(Number{std::to_string(value)}) {}
bool Value::is_bool() const { return std::holds_alternative<bool>(storage); }
bool Value::is_number() const {
  return std::holds_alternative<Number>(storage);
}
bool Value::is_string() const {
  return std::holds_alternative<std::string>(storage);
}
bool Value::is_array() const { return std::holds_alternative<Array>(storage); }
bool Value::is_object() const {
  return std::holds_alternative<Object>(storage);
}
bool Value::is_null() const {
  return std::holds_alternative<std::nullptr_t>(storage);
}
bool Value::is_unsigned_integer() const {
  if (!is_number())
    return false;
  const auto &lexeme = std::get<Number>(storage).lexeme;
  if (lexeme.empty() || lexeme.front() == '-')
    return false;
  std::uint64_t value = 0;
  const auto [end, error] =
      std::from_chars(lexeme.data(), lexeme.data() + lexeme.size(), value);
  return error == std::errc{} && end == lexeme.data() + lexeme.size();
}
std::uint64_t Value::unsigned_integer() const {
  if (!is_unsigned_integer())
    throw Error("invalid_value", "");
  return std::stoull(std::get<Number>(storage).lexeme);
}
const std::string &Value::string() const {
  return std::get<std::string>(storage);
}
const Array &Value::array() const { return std::get<Array>(storage); }
const Object &Value::object() const { return std::get<Object>(storage); }

Error::Error(std::string error_code, std::string error_path)
    : std::runtime_error(error_code + ":" + error_path),
      code(std::move(error_code)), path(std::move(error_path)) {}

Value parse(std::string_view input) { return Parser(input).run(); }

std::string quote(std::string_view ascii) {
  constexpr char hex[] = "0123456789abcdef";
  std::string out = "\"";
  for (const unsigned char c : ascii) {
    if (c > 0x7f)
      throw Error("invalid_value", "");
    if (c == '"')
      out += "\\\"";
    else if (c == '\\')
      out += "\\\\";
    else if (c < 0x20) {
      out += "\\u00";
      out.push_back(hex[c >> 4]);
      out.push_back(hex[c & 0xf]);
    } else
      out.push_back(static_cast<char>(c));
  }
  out.push_back('"');
  return out;
}

std::string canonical(const Value &value) {
  std::string out;
  append_canonical(out, value);
  return out;
}

std::string pointer_child(std::string_view parent, std::string_view key) {
  std::string out(parent);
  out.push_back('/');
  for (const char c : key) {
    if (c == '~')
      out += "~0";
    else if (c == '/')
      out += "~1";
    else
      out.push_back(c);
  }
  return out;
}

} // namespace axissim::gemm_json
