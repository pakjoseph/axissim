#include "study_contract.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <variant>
#include <vector>

#include "config.hpp"
#include "metrics.hpp"
#include "profile.hpp"
#include "simulation.hpp"

namespace axissim {
namespace {

struct Json {
  using Array = std::vector<Json>;
  using Object = std::map<std::string, Json>;
  using Value = std::variant<std::nullptr_t, bool, std::int64_t, std::uint64_t, double, std::string, Array, Object>;
  Value value = nullptr;
  Json() = default;
  Json(std::nullptr_t) : value(nullptr) {}
  Json(bool v) : value(v) {}
  Json(std::int64_t v) : value(v) {}
  Json(std::uint64_t v) : value(v) {}
  Json(int v) : value(static_cast<std::int64_t>(v)) {}
  Json(double v) : value(v) {}
  Json(std::string v) : value(std::move(v)) {}
  Json(const char* v) : value(std::string(v)) {}
  Json(Array v) : value(std::move(v)) {}
  Json(Object v) : value(std::move(v)) {}
  [[nodiscard]] bool is_null() const { return std::holds_alternative<std::nullptr_t>(value); }
  [[nodiscard]] bool is_bool() const { return std::holds_alternative<bool>(value); }
  [[nodiscard]] bool is_number() const { return std::holds_alternative<std::int64_t>(value) || std::holds_alternative<std::uint64_t>(value) || std::holds_alternative<double>(value); }
  [[nodiscard]] bool is_string() const { return std::holds_alternative<std::string>(value); }
  [[nodiscard]] bool is_array() const { return std::holds_alternative<Array>(value); }
  [[nodiscard]] bool is_object() const { return std::holds_alternative<Object>(value); }
  [[nodiscard]] const Object& object() const { return std::get<Object>(value); }
  [[nodiscard]] const Array& array() const { return std::get<Array>(value); }
  [[nodiscard]] const std::string& string() const { return std::get<std::string>(value); }
};

struct ContractError : std::runtime_error {
  std::string code;
  std::string field;
  ContractError(std::string c, std::string f, std::string m)
      : std::runtime_error(std::move(m)), code(std::move(c)), field(std::move(f)) {}
};

[[nodiscard]] bool valid_utf8(std::string_view text) {
  for (std::size_t i = 0; i < text.size();) {
    const auto first = static_cast<unsigned char>(text[i]);
    if (first <= 0x7f) { ++i; continue; }
    std::size_t count = 0;
    std::uint32_t code = 0;
    if ((first & 0xe0) == 0xc0) { count = 2; code = first & 0x1f; }
    else if ((first & 0xf0) == 0xe0) { count = 3; code = first & 0x0f; }
    else if ((first & 0xf8) == 0xf0) { count = 4; code = first & 0x07; }
    else return false;
    if (i + count > text.size()) return false;
    for (std::size_t j = 1; j < count; ++j) {
      const auto next = static_cast<unsigned char>(text[i + j]);
      if ((next & 0xc0) != 0x80) return false;
      code = (code << 6) | (next & 0x3f);
    }
    if ((count == 2 && code < 0x80) || (count == 3 && code < 0x800) || (count == 4 && code < 0x10000)
        || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) return false;
    i += count;
  }
  return true;
}

void append_utf8(std::string& out, std::uint32_t code) {
  if (code <= 0x7f) out.push_back(static_cast<char>(code));
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

class JsonParser {
 public:
  explicit JsonParser(std::string_view input) : input_(input) {
    if (!valid_utf8(input)) throw ContractError("invalid_json", "", "Request JSON must be valid UTF-8.");
  }
  [[nodiscard]] Json parse() {
    skip_space();
    Json result = parse_value();
    skip_space();
    if (position_ != input_.size()) fail();
    return result;
  }
 private:
  [[noreturn]] void fail() const { throw ContractError("invalid_json", "", "Request is not valid JSON."); }
  void skip_space() { while (position_ < input_.size() && (input_[position_] == ' ' || input_[position_] == '\t' || input_[position_] == '\r' || input_[position_] == '\n')) ++position_; }
  [[nodiscard]] char peek() const { return position_ < input_.size() ? input_[position_] : '\0'; }
  bool take(char c) { if (peek() != c) return false; ++position_; return true; }
  [[nodiscard]] Json parse_value() {
    switch (peek()) {
      case '{': return parse_object();
      case '[': return parse_array();
      case '"': return Json(parse_string());
      case 't': if (input_.substr(position_, 4) == "true") { position_ += 4; return Json(true); } break;
      case 'f': if (input_.substr(position_, 5) == "false") { position_ += 5; return Json(false); } break;
      case 'n': if (input_.substr(position_, 4) == "null") { position_ += 4; return Json(nullptr); } break;
      default: if (peek() == '-' || (peek() >= '0' && peek() <= '9')) return parse_number();
    }
    fail();
  }
  [[nodiscard]] Json parse_object() {
    ++position_;
    skip_space();
    Json::Object object;
    if (take('}')) return Json(std::move(object));
    while (true) {
      if (!take('"')) fail();
      --position_;
      std::string key = parse_string();
      skip_space();
      if (!take(':')) fail();
      skip_space();
      auto [it, inserted] = object.emplace(std::move(key), parse_value());
      (void)it;
      if (!inserted) fail();
      skip_space();
      if (take('}')) break;
      if (!take(',')) fail();
      skip_space();
    }
    return Json(std::move(object));
  }
  [[nodiscard]] Json parse_array() {
    ++position_;
    skip_space();
    Json::Array array;
    if (take(']')) return Json(std::move(array));
    while (true) {
      array.push_back(parse_value());
      skip_space();
      if (take(']')) break;
      if (!take(',')) fail();
      skip_space();
    }
    return Json(std::move(array));
  }
  [[nodiscard]] std::uint32_t hex4() {
    std::uint32_t result = 0;
    for (int i = 0; i < 4; ++i) {
      if (position_ >= input_.size()) fail();
      const char c = input_[position_++];
      result <<= 4;
      if (c >= '0' && c <= '9') result |= static_cast<std::uint32_t>(c - '0');
      else if (c >= 'a' && c <= 'f') result |= static_cast<std::uint32_t>(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') result |= static_cast<std::uint32_t>(c - 'A' + 10);
      else fail();
    }
    return result;
  }
  [[nodiscard]] std::string parse_string() {
    if (!take('"')) fail();
    std::string result;
    while (position_ < input_.size()) {
      const char c = input_[position_++];
      if (c == '"') return result;
      if (static_cast<unsigned char>(c) < 0x20) fail();
      if (c != '\\') { result.push_back(c); continue; }
      if (position_ >= input_.size()) fail();
      const char escape = input_[position_++];
      switch (escape) {
        case '"': result.push_back('"'); break;
        case '\\': result.push_back('\\'); break;
        case '/': result.push_back('/'); break;
        case 'b': result.push_back('\b'); break;
        case 'f': result.push_back('\f'); break;
        case 'n': result.push_back('\n'); break;
        case 'r': result.push_back('\r'); break;
        case 't': result.push_back('\t'); break;
        case 'u': {
          std::uint32_t code = hex4();
          if (code >= 0xd800 && code <= 0xdbff) {
            if (position_ + 2 > input_.size() || input_[position_] != '\\' || input_[position_ + 1] != 'u') fail();
            position_ += 2;
            const std::uint32_t low = hex4();
            if (low < 0xdc00 || low > 0xdfff) fail();
            code = 0x10000 + ((code - 0xd800) << 10) + (low - 0xdc00);
          } else if (code >= 0xdc00 && code <= 0xdfff) fail();
          append_utf8(result, code);
          break;
        }
        default: fail();
      }
    }
    fail();
  }
  [[nodiscard]] Json parse_number() {
    const std::size_t start = position_;
    (void)take('-');
    if (take('0')) {
      if (peek() >= '0' && peek() <= '9') fail();
    } else {
      if (peek() < '1' || peek() > '9') fail();
      while (peek() >= '0' && peek() <= '9') ++position_;
    }
    bool fractional = false;
    if (take('.')) {
      fractional = true;
      if (peek() < '0' || peek() > '9') fail();
      while (peek() >= '0' && peek() <= '9') ++position_;
    }
    if (peek() == 'e' || peek() == 'E') {
      fractional = true;
      ++position_;
      if (peek() == '+' || peek() == '-') ++position_;
      if (peek() < '0' || peek() > '9') fail();
      while (peek() >= '0' && peek() <= '9') ++position_;
    }
    const auto token = input_.substr(start, position_ - start);
    if (!fractional) {
      if (!token.empty() && token.front() == '-') {
        std::int64_t value = 0;
        const auto result = std::from_chars(token.data(), token.data() + token.size(), value);
        if (result.ec == std::errc{} && result.ptr == token.data() + token.size()) return Json(value);
      } else {
        std::uint64_t value = 0;
        const auto result = std::from_chars(token.data(), token.data() + token.size(), value);
        if (result.ec == std::errc{} && result.ptr == token.data() + token.size()) return Json(value);
      }
      fail();
    }
    double value = 0.0;
    const auto result = std::from_chars(token.data(), token.data() + token.size(), value, std::chars_format::general);
    if (result.ec != std::errc{} || result.ptr != token.data() + token.size() || !std::isfinite(value)) fail();
    return Json(value);
  }
  std::string_view input_;
  std::size_t position_ = 0;
};

void append_escaped(std::string& out, std::string_view value) {
  out.push_back('"');
  constexpr char hex[] = "0123456789abcdef";
  for (const unsigned char c : value) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) { out += "\\u00"; out.push_back(hex[c >> 4]); out.push_back(hex[c & 0x0f]); }
        else out.push_back(static_cast<char>(c));
    }
  }
  out.push_back('"');
}

void serialize_into(std::string& out, const Json& json) {
  if (json.is_null()) { out += "null"; return; }
  if (const auto* b = std::get_if<bool>(&json.value)) { out += *b ? "true" : "false"; return; }
  if (const auto* i = std::get_if<std::int64_t>(&json.value)) { out += std::to_string(*i); return; }
  if (const auto* u = std::get_if<std::uint64_t>(&json.value)) { out += std::to_string(*u); return; }
  if (const auto* d = std::get_if<double>(&json.value)) {
    if (!std::isfinite(*d)) throw std::runtime_error("Non-finite JSON number");
    std::array<char, 64> buffer{};
    const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), *d, std::chars_format::general, std::numeric_limits<double>::max_digits10);
    if (result.ec != std::errc{}) throw std::runtime_error("Unable to serialize JSON number");
    out.append(buffer.data(), result.ptr);
    return;
  }
  if (const auto* s = std::get_if<std::string>(&json.value)) { append_escaped(out, *s); return; }
  if (const auto* array = std::get_if<Json::Array>(&json.value)) {
    out.push_back('[');
    bool first = true;
    for (const auto& child : *array) { if (!first) out.push_back(','); first = false; serialize_into(out, child); }
    out.push_back(']');
    return;
  }
  const auto& object = std::get<Json::Object>(json.value);
  out.push_back('{');
  bool first = true;
  for (const auto& [key, child] : object) {
    if (!first) out.push_back(','); first = false;
    append_escaped(out, key); out.push_back(':'); serialize_into(out, child);
  }
  out.push_back('}');
}

[[nodiscard]] std::string serialize(const Json& json) { std::string out; serialize_into(out, json); return out; }

class Sha256 {
 public:
  void update(std::string_view data) {
    for (unsigned char c : data) {
      block_[block_size_++] = c;
      bit_count_ += 8;
      if (block_size_ == 64) { transform(); block_size_ = 0; }
    }
  }
  [[nodiscard]] std::string finish() {
    block_[block_size_++] = 0x80;
    if (block_size_ > 56) { while (block_size_ < 64) block_[block_size_++] = 0; transform(); block_size_ = 0; }
    while (block_size_ < 56) block_[block_size_++] = 0;
    for (int i = 7; i >= 0; --i) block_[block_size_++] = static_cast<std::uint8_t>(bit_count_ >> (i * 8));
    transform();
    constexpr char hex[] = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (const auto word : state_) for (int shift = 28; shift >= 0; shift -= 4) out.push_back(hex[(word >> shift) & 0xf]);
    return out;
  }
 private:
  static constexpr std::array<std::uint32_t, 64> k_ = {
      0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
      0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
      0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
      0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
      0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
      0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
      0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
      0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
  void transform() {
    std::array<std::uint32_t, 64> w{};
    for (std::size_t i = 0; i < 16; ++i) w[i] = (static_cast<std::uint32_t>(block_[i*4]) << 24) | (static_cast<std::uint32_t>(block_[i*4+1]) << 16) | (static_cast<std::uint32_t>(block_[i*4+2]) << 8) | block_[i*4+3];
    for (std::size_t i = 16; i < 64; ++i) {
      const auto s0 = std::rotr(w[i-15], 7) ^ std::rotr(w[i-15], 18) ^ (w[i-15] >> 3);
      const auto s1 = std::rotr(w[i-2], 17) ^ std::rotr(w[i-2], 19) ^ (w[i-2] >> 10);
      w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    auto [a,b,c,d,e,f,g,h] = state_;
    for (std::size_t i = 0; i < 64; ++i) {
      const auto s1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
      const auto ch = (e & f) ^ (~e & g);
      const auto temp1 = h + s1 + ch + k_[i] + w[i];
      const auto s0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
      const auto maj = (a & b) ^ (a & c) ^ (b & c);
      const auto temp2 = s0 + maj;
      h=g; g=f; f=e; e=d+temp1; d=c; c=b; b=a; a=temp1+temp2;
    }
    state_[0]+=a; state_[1]+=b; state_[2]+=c; state_[3]+=d; state_[4]+=e; state_[5]+=f; state_[6]+=g; state_[7]+=h;
  }
  std::array<std::uint32_t, 8> state_ = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
  std::array<std::uint8_t, 64> block_{};
  std::size_t block_size_ = 0;
  std::uint64_t bit_count_ = 0;
};

[[nodiscard]] std::string sha256(std::string_view bytes) { Sha256 hash; hash.update(bytes); return hash.finish(); }
[[nodiscard]] std::string read_file(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("file_open");
  std::ostringstream contents;
  contents << input.rdbuf();
  if (!input.good() && !input.eof()) throw std::runtime_error("file_read");
  return contents.str();
}
[[nodiscard]] std::string sha256_file(const std::filesystem::path& path) { return sha256(read_file(path)); }

[[noreturn]] void invalid(std::string code, std::string field, std::string message) {
  throw ContractError(std::move(code), std::move(field), std::move(message));
}
[[nodiscard]] const Json& required(const Json::Object& object, const std::string& key, const std::string& parent) {
  const auto found = object.find(key);
  if (found == object.end()) invalid("missing_field", parent.empty() ? key : parent + "." + key, "A required request field is missing.");
  return found->second;
}
[[nodiscard]] const Json::Object& as_object(const Json& value, const std::string& field) {
  if (!value.is_object()) invalid("invalid_type", field, "Request field must be an object.");
  return value.object();
}
[[nodiscard]] const Json::Array& as_array(const Json& value, const std::string& field) {
  if (!value.is_array()) invalid("invalid_type", field, "Request field must be an array.");
  return value.array();
}
[[nodiscard]] std::string as_string(const Json& value, const std::string& field) {
  if (!value.is_string()) invalid("invalid_type", field, "Request field must be a string.");
  return value.string();
}
[[nodiscard]] double as_number(const Json& value, const std::string& field) {
  double number = 0.0;
  if (const auto* d = std::get_if<double>(&value.value)) number = *d;
  else if (const auto* i = std::get_if<std::int64_t>(&value.value)) number = static_cast<double>(*i);
  else if (const auto* u = std::get_if<std::uint64_t>(&value.value)) number = static_cast<double>(*u);
  else invalid("invalid_type", field, "Request field must be a finite number.");
  if (!std::isfinite(number)) invalid("invalid_type", field, "Request field must be a finite number.");
  return number;
}
[[nodiscard]] std::uint64_t as_uint(const Json& value, const std::string& field) {
  if (const auto* u = std::get_if<std::uint64_t>(&value.value)) return *u;
  if (const auto* i = std::get_if<std::int64_t>(&value.value); i != nullptr && *i >= 0) return static_cast<std::uint64_t>(*i);
  invalid("invalid_type", field, "Request field must be a nonnegative JSON integer.");
}
[[nodiscard]] bool as_bool(const Json& value, const std::string& field) {
  if (!value.is_bool()) invalid("invalid_type", field, "Request field must be a boolean.");
  return std::get<bool>(value.value);
}
void exact_keys(const Json::Object& object, const std::set<std::string>& expected, const std::string& field) {
  for (const auto& key : expected) if (!object.contains(key)) invalid("missing_field", field.empty() ? key : field + "." + key, "A required request field is missing.");
  for (const auto& [key, value] : object) { (void)value; if (!expected.contains(key)) invalid("unknown_field", field.empty() ? key : field + "." + key, "Request contains an unsupported field."); }
}
[[nodiscard]] bool is_sha256(std::string_view value) {
  return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}
[[nodiscard]] bool is_ascii_token(std::string_view value, std::size_t max_length = 128) {
  return !value.empty() && value.size() <= max_length && std::all_of(value.begin(), value.end(), [](unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.' || c == ':';
  });
}
[[nodiscard]] std::string canonical_path(const Json& value, const std::string& field) {
  const std::string path_text = as_string(value, field);
  if (path_text.empty() || path_text.size() > 512 || path_text.find('\\') != std::string::npos || path_text.front() == '/')
    invalid("invalid_path", field, "Path must be repository-relative and use forward slashes.");
  const std::filesystem::path path(path_text);
  if (path.is_absolute()) invalid("invalid_path", field, "Path must be repository-relative and use forward slashes.");
  for (const auto& part : path) if (part == ".." || part == ".") invalid("invalid_path", field, "Path must be normalized and must not traverse parent directories.");
  if (path.lexically_normal().generic_string() != path_text) invalid("invalid_path", field, "Path must be normalized and must not traverse parent directories.");
  return path_text;
}
[[nodiscard]] std::filesystem::path repo_file(const std::filesystem::path& root, const std::string& relative, const std::string& field) {
  const auto path = std::filesystem::canonical(root / std::filesystem::path(relative));
  const auto canonical_root = std::filesystem::canonical(root);
  const auto rel = path.lexically_relative(canonical_root);
  if (rel.empty() || rel.is_absolute() || rel.generic_string().starts_with("..")) invalid("invalid_path", field, "Input path resolves outside the repository.");
  return path;
}
[[nodiscard]] std::string scalar_config_value(const Json& value, const std::string& field) {
  if (value.is_string()) return value.string();
  if (const auto* i = std::get_if<std::int64_t>(&value.value)) return std::to_string(*i);
  if (const auto* u = std::get_if<std::uint64_t>(&value.value)) return std::to_string(*u);
  if (const auto* d = std::get_if<double>(&value.value)) {
    if (!std::isfinite(*d)) invalid("invalid_type", field, "Configuration override must be a finite scalar.");
    std::array<char, 64> buffer{};
    const auto r = std::to_chars(buffer.data(), buffer.data() + buffer.size(), *d, std::chars_format::general, std::numeric_limits<double>::max_digits10);
    return std::string(buffer.data(), r.ptr);
  }
  invalid("invalid_type", field, "Configuration override must be a scalar string or number.");
}

struct FactorDeclaration {
  std::string id;
  std::string treatment;
  std::string evidence;
  std::string rationale;
};
struct CandidateRequest {
  Json::Object root;
  Json::Object identities;
  std::string manifest_id;
  std::string candidate_id;
  std::string simulator_build_identity;
  std::string configuration_identity;
  std::string workload_identity;
  std::string model_assumption_identity;
  std::string measurement_protocol_identity;
  std::string workload_id;
  std::string workload_mode;
  std::string release_stratum_id;
  std::string base_config_path;
  std::string base_config_sha256;
  std::map<std::string, std::string> declared_include_hashes;
  std::map<std::string, std::string> overrides;
  Json::Object protocol;
  Json::Object release_schedule;
  Json::Object extensions;
  bool extensions_present = false;
  std::vector<FactorDeclaration> factors;
  StudyMeasurementRequest observer_request;
  std::uint64_t max_retained_trace_bytes = 0;
  std::uint64_t max_candidate_result_bytes = 0;
  bool retain_trace = false;
  Json power_overlay = nullptr;
};

[[nodiscard]] std::string digest_field(const Json::Object& object, const std::string& key, const std::string& parent = "") {
  const std::string value = as_string(required(object, key, parent), parent.empty() ? key : parent + "." + key);
  if (!is_sha256(value)) invalid("identity_mismatch", parent.empty() ? key : parent + "." + key, "Identity must be a lowercase SHA-256 digest.");
  return value;
}

[[nodiscard]] CandidateRequest parse_request(const Json& json) {
  CandidateRequest request;
  request.root = as_object(json, "");
  const std::set<std::string> top_keys = {"schema_id","manifest_id","candidate_id","simulator_build_identity","configuration_identity","workload_identity","model_assumption_identity","measurement_protocol_identity","configuration","workload","measurement_protocol","limits","power_overlay","factor_coverage","trace_policy","extensions"};
  for (const auto& key : top_keys) if (key != "extensions" && !request.root.contains(key)) invalid("missing_field", key, "A required request field is missing.");
  for (const auto& [key, value] : request.root) { (void)value; if (!top_keys.contains(key)) invalid("unknown_field", key, "Request contains an unsupported field."); }
  const std::string schema = as_string(required(request.root, "schema_id", ""), "schema_id");
  if (schema != "axissim.study-candidate-request.v1") invalid("unsupported_schema", "schema_id", "Request schema is unsupported.");
  request.manifest_id = digest_field(request.root, "manifest_id");
  request.candidate_id = digest_field(request.root, "candidate_id");
  request.simulator_build_identity = digest_field(request.root, "simulator_build_identity");
  request.configuration_identity = digest_field(request.root, "configuration_identity");
  request.workload_identity = digest_field(request.root, "workload_identity");
  request.model_assumption_identity = digest_field(request.root, "model_assumption_identity");
  request.measurement_protocol_identity = digest_field(request.root, "measurement_protocol_identity");

  const auto& config = as_object(required(request.root, "configuration", ""), "configuration");
  exact_keys(config, {"base_config_path","base_config_sha256","included_config_hashes","overrides","expected_it_power_budget_w"}, "configuration");
  request.base_config_path = canonical_path(required(config, "base_config_path", "configuration"), "configuration.base_config_path");
  request.base_config_sha256 = digest_field(config, "base_config_sha256", "configuration");
  const auto& included = as_object(required(config, "included_config_hashes", "configuration"), "configuration.included_config_hashes");
  for (const auto& [path, hash] : included) {
    const auto normalized = canonical_path(Json(path), "configuration.included_config_hashes");
    const auto digest = as_string(hash, "configuration.included_config_hashes." + path);
    if (!is_sha256(digest)) invalid("identity_mismatch", "configuration.included_config_hashes." + path, "Identity must be a lowercase SHA-256 digest.");
    request.declared_include_hashes.emplace(normalized, digest);
  }
  const auto& overrides = as_object(required(config, "overrides", "configuration"), "configuration.overrides");
  for (const auto& [key, value] : overrides) {
    if (!supported_keys().contains(key)) invalid("config_error", "configuration.overrides." + key, "Configuration override key is unsupported.");
    request.overrides.emplace(key, scalar_config_value(value, "configuration.overrides." + key));
  }
  if (as_number(required(config, "expected_it_power_budget_w", "configuration"), "configuration.expected_it_power_budget_w") != 0.23)
    invalid("study_budget_mismatch", "configuration.expected_it_power_budget_w", "Study candidate must declare the fixed 0.23 W budget.");

  const auto& workload = as_object(required(request.root, "workload", ""), "workload");
  exact_keys(workload, {"workload_id","workload_mode","profile_path","profile_sha256","mapping_path","mapping_sha256","release_stratum_id","release_schedule"}, "workload");
  request.workload_id = as_string(required(workload, "workload_id", "workload"), "workload.workload_id");
  request.workload_mode = as_string(required(workload, "workload_mode", "workload"), "workload.workload_mode");
  request.release_stratum_id = as_string(required(workload, "release_stratum_id", "workload"), "workload.release_stratum_id");
  if (!is_ascii_token(request.workload_id) || !is_ascii_token(request.release_stratum_id)) invalid("invalid_type", "workload", "Workload identifiers must be bounded ASCII tokens.");
  if (request.workload_mode != "physical_ai" && request.workload_mode != "profiled_graph") invalid("invalid_enum", "workload.workload_mode", "Workload mode is not supported by candidate API v1.");
  const Json& profile_path = required(workload, "profile_path", "workload");
  const Json& profile_hash = required(workload, "profile_sha256", "workload");
  const Json& mapping_path = required(workload, "mapping_path", "workload");
  const Json& mapping_hash = required(workload, "mapping_sha256", "workload");
  std::optional<std::string> profile_path_value;
  std::optional<std::string> profile_hash_value;
  std::optional<std::string> mapping_path_value;
  std::optional<std::string> mapping_hash_value;
  if (!profile_path.is_null()) profile_path_value = canonical_path(profile_path, "workload.profile_path");
  if (!profile_hash.is_null()) { profile_hash_value = as_string(profile_hash, "workload.profile_sha256"); if (!is_sha256(*profile_hash_value)) invalid("identity_mismatch", "workload.profile_sha256", "Identity must be a lowercase SHA-256 digest."); }
  if (!mapping_path.is_null()) mapping_path_value = canonical_path(mapping_path, "workload.mapping_path");
  if (!mapping_hash.is_null()) { mapping_hash_value = as_string(mapping_hash, "workload.mapping_sha256"); if (!is_sha256(*mapping_hash_value)) invalid("identity_mismatch", "workload.mapping_sha256", "Identity must be a lowercase SHA-256 digest."); }
  if (profile_path_value.has_value() != profile_hash_value.has_value() || mapping_path_value.has_value() != mapping_hash_value.has_value())
    invalid("workload_input_mismatch", "workload", "Workload source paths and digests must be supplied together.");

  request.release_schedule = as_object(required(workload, "release_schedule", "workload"), "workload.release_schedule");
  const auto release_kind = as_string(required(request.release_schedule, "kind", "workload.release_schedule"), "workload.release_schedule.kind");
  if (release_kind == "periodic") {
    exact_keys(request.release_schedule, {"kind","period_us"}, "workload.release_schedule");
    const double period = as_number(required(request.release_schedule, "period_us", "workload.release_schedule"), "workload.release_schedule.period_us");
    if (period <= 0.0) invalid("invalid_type", "workload.release_schedule.period_us", "Periodic release interval must be positive.");
  } else if (release_kind == "trace") {
    exact_keys(request.release_schedule, {"kind","path","sha256"}, "workload.release_schedule");
    (void)canonical_path(required(request.release_schedule, "path", "workload.release_schedule"), "workload.release_schedule.path");
    (void)digest_field(request.release_schedule, "sha256", "workload.release_schedule");
  } else invalid("invalid_enum", "workload.release_schedule.kind", "Release schedule kind is not supported.");

  request.power_overlay = required(request.root, "power_overlay", "");
  if (!request.power_overlay.is_null()) {
    const auto& overlay = as_object(request.power_overlay, "power_overlay");
    exact_keys(overlay, {"source_path","source_sha256","fields"}, "power_overlay");
    (void)canonical_path(required(overlay, "source_path", "power_overlay"), "power_overlay.source_path");
    (void)digest_field(overlay, "source_sha256", "power_overlay");
    const auto& fields = as_object(required(overlay, "fields", "power_overlay"), "power_overlay.fields");
    for (const auto& [key, value] : fields) { (void)value; if (!supported_keys().contains(key)) invalid("config_error", "power_overlay.fields." + key, "Power overlay field is unsupported."); }
  }

  request.protocol = as_object(required(request.root, "measurement_protocol", ""), "measurement_protocol");
  const std::string mode = as_string(required(request.protocol, "mode", "measurement_protocol"), "measurement_protocol.mode");
  if (mode != "finite" && mode != "sustained") invalid("invalid_enum", "measurement_protocol.mode", "Measurement mode is unsupported.");
  const double origin = as_number(required(request.protocol, "time_origin_us", "measurement_protocol"), "measurement_protocol.time_origin_us");
  if (origin != 0.0) invalid("invalid_type", "measurement_protocol.time_origin_us", "Measurement time origin must be zero.");
  request.observer_request.mode = mode == "finite" ? StudyMeasurementMode::Finite : StudyMeasurementMode::Sustained;
  std::set<std::string> protocol_keys{"mode","time_origin_us","deadline_policy","percentile_method"};
  if (mode == "sustained") {
    protocol_keys.insert("warmup_end_us"); protocol_keys.insert("window_start_us"); protocol_keys.insert("window_end_us");
    request.observer_request.window_start_us = as_number(required(request.protocol, "window_start_us", "measurement_protocol"), "measurement_protocol.window_start_us");
    request.observer_request.window_end_us = as_number(required(request.protocol, "window_end_us", "measurement_protocol"), "measurement_protocol.window_end_us");
    const double warmup = as_number(required(request.protocol, "warmup_end_us", "measurement_protocol"), "measurement_protocol.warmup_end_us");
    if (warmup < 0.0 || warmup > request.observer_request.window_start_us || request.observer_request.window_start_us < 0.0 || request.observer_request.window_end_us <= request.observer_request.window_start_us)
      invalid("invalid_type", "measurement_protocol", "Sustained protocol boundaries are inconsistent.");
  }
  exact_keys(request.protocol, protocol_keys, "measurement_protocol");
  if (as_string(required(request.protocol, "percentile_method", "measurement_protocol"), "measurement_protocol.percentile_method") != "linear_n_minus_1_p99")
    invalid("invalid_enum", "measurement_protocol.percentile_method", "Only the documented linear n-minus-one P99 method is supported.");
  const auto& deadlines = as_object(required(request.protocol, "deadline_policy", "measurement_protocol"), "measurement_protocol.deadline_policy");
  exact_keys(deadlines, {"frame","control"}, "measurement_protocol.deadline_policy");
  for (const auto& [name, policy_json] : deadlines) {
    const auto& policy = as_object(policy_json, "measurement_protocol.deadline_policy." + name);
    exact_keys(policy, {"relative_deadline_us","handling"}, "measurement_protocol.deadline_policy." + name);
    const Json& deadline = required(policy, "relative_deadline_us", "measurement_protocol.deadline_policy." + name);
    const std::string handling = as_string(required(policy, "handling", "measurement_protocol.deadline_policy." + name), "measurement_protocol.deadline_policy." + name + ".handling");
    if (handling != "hard_constraint" && handling != "objective" && handling != "not_applicable") invalid("invalid_enum", "measurement_protocol.deadline_policy." + name + ".handling", "Deadline handling is unsupported.");
    if (deadline.is_null()) { if (handling != "not_applicable") invalid("invalid_type", "measurement_protocol.deadline_policy." + name, "A null deadline must be marked not_applicable."); }
    else { if (as_number(deadline, "measurement_protocol.deadline_policy." + name + ".relative_deadline_us") <= 0.0 || handling == "not_applicable") invalid("invalid_type", "measurement_protocol.deadline_policy." + name, "A configured deadline must be positive and applicable."); }
  }

  const auto& limits = as_object(required(request.root, "limits", ""), "limits");
  exact_keys(limits, {"max_candidates","max_task_events_per_candidate","max_backlog_work_units","max_retained_trace_bytes","max_candidate_result_bytes","max_study_output_bytes","max_simulated_time_us","max_drain_simulated_us","candidate_wall_timeout_seconds"}, "limits");
  for (const auto& [key, value] : limits) {
    if (key == "max_simulated_time_us" || key == "max_drain_simulated_us" || key == "candidate_wall_timeout_seconds") {
      if (as_number(value, "limits." + key) <= 0.0) invalid("invalid_type", "limits." + key, "Limit must be positive.");
    } else if (as_uint(value, "limits." + key) == 0) invalid("invalid_type", "limits." + key, "Limit must be positive.");
  }
  request.observer_request.limits.max_task_events = as_uint(required(limits, "max_task_events_per_candidate", "limits"), "limits.max_task_events_per_candidate");
  request.observer_request.limits.max_backlog_work_units = as_uint(required(limits, "max_backlog_work_units", "limits"), "limits.max_backlog_work_units");
  request.observer_request.limits.max_simulated_time_us = as_number(required(limits, "max_simulated_time_us", "limits"), "limits.max_simulated_time_us");
  request.observer_request.limits.max_drain_simulated_us = as_number(required(limits, "max_drain_simulated_us", "limits"), "limits.max_drain_simulated_us");
  request.max_retained_trace_bytes = as_uint(required(limits, "max_retained_trace_bytes", "limits"), "limits.max_retained_trace_bytes");
  request.max_candidate_result_bytes = as_uint(required(limits, "max_candidate_result_bytes", "limits"), "limits.max_candidate_result_bytes");
  if (mode == "sustained" && request.observer_request.window_end_us > request.observer_request.limits.max_simulated_time_us)
    invalid("invalid_type", "measurement_protocol.window_end_us", "Observation window exceeds the model-time limit.");

  const auto& trace_policy = as_object(required(request.root, "trace_policy", ""), "trace_policy");
  exact_keys(trace_policy, {"retain_full_trace","max_trace_bytes"}, "trace_policy");
  request.retain_trace = as_bool(required(trace_policy, "retain_full_trace", "trace_policy"), "trace_policy.retain_full_trace");
  const auto trace_bytes = as_uint(required(trace_policy, "max_trace_bytes", "trace_policy"), "trace_policy.max_trace_bytes");
  if (trace_bytes == 0 || trace_bytes > request.max_retained_trace_bytes) invalid("invalid_type", "trace_policy.max_trace_bytes", "Per-candidate trace cap must be positive and within the study cap.");

  const auto& factors = as_array(required(request.root, "factor_coverage", ""), "factor_coverage");
  static const std::set<std::string> factor_ids = {"npu_macs","rvv_lanes","dram_bandwidth_gbps","sram_capacity_bytes","weight_sram_capacity_bytes","npu_weights_resident","npu_tile_m","npu_tile_n","npu_tile_k","scheduler_policy","rvv_chunk_elements","rvv_frequency_ghz","npu_frequency_ghz","npu_arithmetic_energy_scale","arithmetic_voltage_ratio","release_schedule","frame_period_us","control_period_us","deadline_us","control_deadline_us"};
  std::string previous_factor;
  for (std::size_t index = 0; index < factors.size(); ++index) {
    const auto& record = as_object(factors[index], "factor_coverage[" + std::to_string(index) + "]");
    const auto path = "factor_coverage[" + std::to_string(index) + "]";
    exact_keys(record, {"factor_id","treatment","evidence_source","rationale"}, path);
    FactorDeclaration factor{as_string(required(record,"factor_id",path),path+".factor_id"), as_string(required(record,"treatment",path),path+".treatment"), as_string(required(record,"evidence_source",path),path+".evidence_source"), as_string(required(record,"rationale",path),path+".rationale")};
    if (!factor_ids.contains(factor.id) || !is_ascii_token(factor.id)) invalid("invalid_enum", path+".factor_id", "Factor identifier is not registered in schema v1.");
    if (factor.treatment != "swept" && factor.treatment != "held_constant") invalid("invalid_enum", path+".treatment", "Factor treatment is unsupported.");
    if (factor.evidence.size() > 256 || factor.rationale.size() > 256 || !valid_utf8(factor.evidence) || !valid_utf8(factor.rationale)) invalid("invalid_type", path, "Factor text exceeds its UTF-8 byte limit.");
    if (!previous_factor.empty() && factor.id <= previous_factor) invalid("invalid_type", path+".factor_id", "Factor declarations must be unique and sorted by factor_id.");
    previous_factor = factor.id;
    request.factors.push_back(std::move(factor));
  }

  if (request.root.contains("extensions")) {
    request.extensions_present = true;
    request.extensions = as_object(request.root.at("extensions"), "extensions");
    for (const auto& [key, value] : request.extensions) {
      if (key.size() > 128 || key.size() < 3 || key.find('.') == std::string::npos || !std::all_of(key.begin(), key.end(), [](unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-'; }) || !value.is_object())
        invalid("invalid_type", "extensions." + key, "Extension keys must be reverse-DNS tokens and values must be objects.");
      if (serialize(value).size() > 4096) invalid("invalid_type", "extensions." + key, "Extension value exceeds the v1 byte limit.");
    }
  }
  return request;
}

[[nodiscard]] std::string repo_relative(const std::filesystem::path& root, const std::string& value) {
  if (value.empty()) return "";
  const auto absolute = std::filesystem::canonical(value);
  const auto canonical_root = std::filesystem::canonical(root);
  const auto relative = absolute.lexically_relative(canonical_root);
  if (relative.empty() || relative.is_absolute() || relative.generic_string().starts_with("..")) throw std::runtime_error("resolved path outside repository");
  return relative.generic_string();
}

[[nodiscard]] Json config_json(const Config& c, const std::filesystem::path& root) {
  Json::Object values;
#define PUT_CONFIG(field) values.emplace(#field, Json(c.field))
  PUT_CONFIG(scenario_name); PUT_CONFIG(model_version); PUT_CONFIG(workload_mode);
  values.emplace("workload_profile", Json(c.workload_profile.empty() ? "" : repo_relative(root, c.workload_profile)));
  values.emplace("mapping_plan", Json(c.mapping_plan.empty() ? "" : repo_relative(root, c.mapping_plan)));
  PUT_CONFIG(scheduler_policy); PUT_CONFIG(frame_arrival_mode);
  values.emplace("frame_arrival_trace", Json(c.frame_arrival_trace.empty() ? "" : repo_relative(root, c.frame_arrival_trace)));
  values.emplace("frame_arrival_provenance", Json(c.frame_arrival_provenance.empty() ? "" : repo_relative(root, c.frame_arrival_provenance)));
  PUT_CONFIG(frames); PUT_CONFIG(frame_period_us); PUT_CONFIG(deadline_us); PUT_CONFIG(dram_bandwidth_gbps); PUT_CONFIG(dma_setup_us);
  PUT_CONFIG(control_period_us); PUT_CONFIG(control_deadline_us); PUT_CONFIG(control_ops); PUT_CONFIG(rvv_vlen_bits); PUT_CONFIG(rvv_sew_bits);
  PUT_CONFIG(rvv_lmul); PUT_CONFIG(rvv_lanes); PUT_CONFIG(rvv_ops_per_lane_cycle); PUT_CONFIG(rvv_frequency_ghz); PUT_CONFIG(rvv_stripmine_overhead_cycles);
  PUT_CONFIG(rvv_timing_model); PUT_CONFIG(rvv_affine_overhead_cycles); PUT_CONFIG(rvv_compute_cycle_scale); PUT_CONFIG(rvv_chunk_elements);
  PUT_CONFIG(rvv_memory_accounting); PUT_CONFIG(rvv_memory_bytes_per_element); PUT_CONFIG(npu_macs); PUT_CONFIG(npu_frequency_ghz);
  PUT_CONFIG(input_bytes); PUT_CONFIG(preprocess_elements); PUT_CONFIG(preprocess_ops_per_element); PUT_CONFIG(preprocess_output_bytes);
  PUT_CONFIG(gemm_m); PUT_CONFIG(gemm_n); PUT_CONFIG(gemm_k); PUT_CONFIG(npu_tile_m); PUT_CONFIG(npu_tile_n); PUT_CONFIG(npu_tile_k);
  PUT_CONFIG(npu_element_bytes); PUT_CONFIG(npu_accumulator_bytes); PUT_CONFIG(sram_capacity_bytes); PUT_CONFIG(npu_weights_resident);
  PUT_CONFIG(weight_sram_capacity_bytes); PUT_CONFIG(dma_double_buffering); PUT_CONFIG(postprocess_elements); PUT_CONFIG(postprocess_ops_per_element);
  PUT_CONFIG(postprocess_bytes); PUT_CONFIG(vector_op_pj); PUT_CONFIG(npu_mac_pj); PUT_CONFIG(npu_arithmetic_energy_scale);
  PUT_CONFIG(arithmetic_voltage_ratio); PUT_CONFIG(dram_byte_pj); PUT_CONFIG(dma_byte_pj); PUT_CONFIG(static_power_w);
  PUT_CONFIG(npu_static_power_per_mac_w); PUT_CONFIG(rvv_static_power_per_lane_w); PUT_CONFIG(sram_static_power_per_kib_w);
  PUT_CONFIG(dram_interface_static_power_w); PUT_CONFIG(it_power_budget_w); PUT_CONFIG(pue); PUT_CONFIG(electricity_cost_per_kwh);
  PUT_CONFIG(deployment_xpu_count); PUT_CONFIG(deployment_hours); PUT_CONFIG(base_area_mm2); PUT_CONFIG(npu_area_per_mac_mm2);
  PUT_CONFIG(rvv_area_per_lane_mm2); PUT_CONFIG(sram_area_per_kib_mm2); PUT_CONFIG(max_area_mm2); PUT_CONFIG(ambient_temp_c);
  PUT_CONFIG(thermal_resistance_c_per_w); PUT_CONFIG(thermal_capacitance_j_per_c); PUT_CONFIG(max_junction_temp_c);
  PUT_CONFIG(reliability_fit_rate); PUT_CONFIG(reliability_voltage_fit_multiplier);
#undef PUT_CONFIG
  return Json(std::move(values));
}

struct ResolvedCandidate {
  Config config;
  std::filesystem::path root;
  Json effective_configuration;
  Json power_coverage;
  WorkloadProfile profile;
  bool has_profile = false;
  std::vector<double> frame_arrivals_us;
  std::uint64_t max_trace_bytes = 0;
};

struct CoverageTerms {
  std::vector<std::string> represented;
  std::vector<std::string> omitted;
  Json::Object coefficients;
};

void cost_term(CoverageTerms& terms, const std::string& id, double coefficient) {
  terms.coefficients.emplace(id, Json(coefficient));
  (coefficient > 0.0 ? terms.represented : terms.omitted).push_back(id);
}
void missing_term(CoverageTerms& terms, const std::string& id) { terms.omitted.push_back(id); }

[[nodiscard]] bool profile_uses_class(const WorkloadProfile& profile, std::string_view klass) {
  return std::any_of(profile.operators.begin(), profile.operators.end(), [klass](const ProfileOperator& op) {
    return op.mapping_class == klass || (klass == "npu" && op.macs > 0) || (klass == "vector" && op.vector_elements > 0);
  });
}
[[nodiscard]] bool profile_has_resident_weights(const WorkloadProfile& profile) {
  return std::any_of(profile.operators.begin(), profile.operators.end(), [](const ProfileOperator& op) { return op.persistent_weights; });
}

[[nodiscard]] Json build_power_coverage(const CandidateRequest& request, const Config& config,
                                        const WorkloadProfile* profile) {
  const bool physical = config.workload_mode == "physical_ai";
  const bool profile_npu = profile != nullptr && profile_uses_class(*profile, "npu");
  const bool profile_vector = profile != nullptr && profile_uses_class(*profile, "vector");
  const bool has_npu = physical || profile_npu;
  const bool has_vector = physical || profile_vector;
  const bool has_memory = physical || profile != nullptr;
  const bool has_profile_residency = profile != nullptr && profile_has_resident_weights(*profile);
  Json::Array records;
  for (const auto& factor : request.factors) {
    bool applicable = true;
    bool hardware_cost_factor = true;
    CoverageTerms terms;
    const auto base_static = config.static_power_w;
    if (factor.id == "npu_macs") {
      applicable = has_npu;
      if (applicable) cost_term(terms, "static.npu_w_per_mac", config.npu_static_power_per_mac_w);
    } else if (factor.id == "rvv_lanes") {
      applicable = has_vector;
      if (applicable) cost_term(terms, "static.rvv_w_per_lane", config.rvv_static_power_per_lane_w);
    } else if (factor.id == "dram_bandwidth_gbps") {
      applicable = has_memory;
      if (applicable) {
        cost_term(terms, "static.dram_interface_w", config.dram_interface_static_power_w);
        cost_term(terms, "dynamic.dram_byte_pj_per_byte", config.dram_byte_pj);
        cost_term(terms, "dynamic.dma_byte_pj_per_byte", config.dma_byte_pj);
        missing_term(terms, "missing.static.dram_interface_w_per_gbps");
      }
    } else if (factor.id == "sram_capacity_bytes") {
      applicable = physical;
      if (applicable) {
        cost_term(terms, "static.sram_w_per_kib", config.sram_static_power_per_kib_w);
        missing_term(terms, "missing.dynamic.sram_access_j_per_byte");
      }
    } else if (factor.id == "weight_sram_capacity_bytes" || factor.id == "npu_weights_resident") {
      applicable = physical || has_profile_residency;
      if (applicable) {
        cost_term(terms, "dynamic.dram_byte_pj_per_byte", config.dram_byte_pj);
        cost_term(terms, "dynamic.dma_byte_pj_per_byte", config.dma_byte_pj);
        missing_term(terms, "missing.static.resident_weight_sram_w_per_byte");
        missing_term(terms, "missing.dynamic.resident_weight_sram_access_j_per_byte");
      }
    } else if (factor.id == "npu_tile_m" || factor.id == "npu_tile_n" || factor.id == "npu_tile_k") {
      applicable = physical;
      if (applicable) {
        cost_term(terms, "dynamic.dram_byte_pj_per_byte", config.dram_byte_pj);
        cost_term(terms, "dynamic.dma_byte_pj_per_byte", config.dma_byte_pj);
        missing_term(terms, "missing.dynamic.sram_access_j_per_byte");
      }
    } else if (factor.id == "scheduler_policy" || factor.id == "rvv_chunk_elements") {
      applicable = factor.id == "scheduler_policy" || physical;
      if (applicable) {
        cost_term(terms, "static.base_power_w", base_static);
        if (has_vector) cost_term(terms, "dynamic.vector_op_pj_per_op", config.vector_op_pj);
        if (has_npu) cost_term(terms, "dynamic.npu_mac_pj_per_mac", config.npu_mac_pj);
        if (has_memory) { cost_term(terms, "dynamic.dram_byte_pj_per_byte", config.dram_byte_pj); cost_term(terms, "dynamic.dma_byte_pj_per_byte", config.dma_byte_pj); }
        missing_term(terms, "missing.dynamic.scheduler_j_per_event");
      }
    } else if (factor.id == "rvv_frequency_ghz" || factor.id == "npu_frequency_ghz") {
      applicable = factor.id == "rvv_frequency_ghz" ? has_vector : has_npu;
      if (applicable) {
        cost_term(terms, "static.base_power_w", base_static);
        if (has_vector) cost_term(terms, "static.rvv_w_per_lane", config.rvv_static_power_per_lane_w);
        if (has_npu) cost_term(terms, "static.npu_w_per_mac", config.npu_static_power_per_mac_w);
        missing_term(terms, "missing.dynamic.frequency_voltage_coupling");
        missing_term(terms, "missing.static.frequency_voltage_leakage_coupling");
      }
    } else if (factor.id == "npu_arithmetic_energy_scale") {
      applicable = has_npu;
      if (applicable) cost_term(terms, "dynamic.npu_mac_pj_per_mac", config.npu_mac_pj);
    } else if (factor.id == "arithmetic_voltage_ratio") {
      applicable = has_npu;
      if (applicable) {
        cost_term(terms, "dynamic.npu_mac_pj_per_mac", config.npu_mac_pj);
        missing_term(terms, "missing.dynamic.frequency_voltage_coupling");
        missing_term(terms, "missing.static.frequency_voltage_leakage_coupling");
      }
    } else if (factor.id == "control_period_us" || factor.id == "control_deadline_us") {
      applicable = config.control_period_us > 0.0;
      hardware_cost_factor = false;
    } else if (factor.id == "release_schedule" || factor.id == "frame_period_us" || factor.id == "deadline_us") {
      hardware_cost_factor = false;
    }
    std::sort(terms.represented.begin(), terms.represented.end());
    std::sort(terms.omitted.begin(), terms.omitted.end());
    const std::string coverage = !applicable || !hardware_cost_factor ? "n/a" : terms.omitted.empty() ? "complete"
        : terms.represented.empty() ? "missing" : "partial";
    const Json::Array represented = [&] { Json::Array a; for (const auto& x : terms.represented) a.emplace_back(x); return a; }();
    const Json::Array omitted = [&] { Json::Array a; for (const auto& x : terms.omitted) a.emplace_back(x); return a; }();
    records.emplace_back(Json::Object{{"factor_id",Json(factor.id)}, {"applicability",Json(applicable ? "modeled" : "n/a")},
      {"treatment",Json(factor.treatment)}, {"power_coverage",Json(coverage)}, {"represented_terms",Json(represented)},
      {"omitted_terms",Json(omitted)}, {"resolved_coefficients",Json(std::move(terms.coefficients))},
      {"evidence_source",Json(factor.evidence)}, {"rationale",Json(factor.rationale)}});
  }
  return Json::Object{{"schema_id",Json("axissim.power-coverage.v1")}, {"factors",Json(std::move(records))}};
}

[[nodiscard]] bool looks_like_mnist12(const CandidateRequest& request) {
  const auto& workload = request.root.at("workload").object();
  const auto& profile = workload.at("profile_path");
  return request.workload_id == "mnist12" || (profile.is_string() && std::filesystem::path(profile.string()).filename() == "mnist12_profile.json");
}

[[nodiscard]] ResolvedCandidate resolve_candidate(const CandidateRequest& request, const std::filesystem::path& root,
                                                  const std::string& executable_path) {
  if (sha256_file(std::filesystem::canonical(executable_path)) != request.simulator_build_identity)
    invalid("identity_mismatch", "simulator_build_identity", "Simulator executable digest does not match the request.");

  const auto base_file = repo_file(root, request.base_config_path, "configuration.base_config_path");
  const auto loaded = load_config_with_sources(base_file.string(), request.overrides);
  const auto canonical_root = std::filesystem::canonical(root);
  const auto loaded_base_path = std::filesystem::absolute(base_file).lexically_normal();
  const auto base_contents = loaded.source_contents.find(loaded_base_path);
  if (base_contents == loaded.source_contents.end() || sha256(base_contents->second) != request.base_config_sha256)
    invalid("configuration_source_mismatch", "configuration.base_config_sha256", "Base configuration bytes do not match the request.");
  std::map<std::string, std::string> actual_includes;
  for (const auto& source : loaded.source_files) {
    const auto canonical_source = std::filesystem::canonical(source);
    const auto relative = canonical_source.lexically_relative(canonical_root);
    if (relative.empty() || relative.is_absolute() || relative.generic_string().starts_with(".."))
      invalid("configuration_source_mismatch", "configuration.included_config_hashes", "A loaded configuration source resolves outside the repository.");
    const auto loaded_bytes = loaded.source_contents.find(source.lexically_normal());
    if (loaded_bytes == loaded.source_contents.end()) invalid("configuration_source_mismatch", "configuration.included_config_hashes", "Loaded configuration source bytes are unavailable.");
    if (canonical_source != std::filesystem::canonical(base_file)) actual_includes.emplace(relative.generic_string(), sha256(loaded_bytes->second));
  }
  if (actual_includes != request.declared_include_hashes)
    invalid("configuration_source_mismatch", "configuration.included_config_hashes", "Loaded configuration include paths or bytes do not match the request.");

  Config config = loaded.config;
  const bool mnist = looks_like_mnist12(request);
  if (mnist) {
    if (request.power_overlay.is_null()) invalid("workload_input_mismatch", "power_overlay", "MNIST-12 requires its declared study-only power overlay.");
    const auto& overlay = request.power_overlay.object();
    constexpr std::string_view overlay_path = "configs/power_budget/manifest.conf";
    if (as_string(overlay.at("source_path"), "power_overlay.source_path") != overlay_path)
      invalid("workload_input_mismatch", "power_overlay.source_path", "MNIST-12 overlay source is not the approved study manifest.");
    const auto overlay_file = repo_file(root, std::string(overlay_path), "power_overlay.source_path");
    const auto overlay_load = load_config_with_sources(overlay_file.string());
    const auto overlay_loaded_bytes = overlay_load.source_contents.find(std::filesystem::absolute(overlay_file).lexically_normal());
    if (overlay_loaded_bytes == overlay_load.source_contents.end() || sha256(overlay_loaded_bytes->second) != as_string(overlay.at("source_sha256"), "power_overlay.source_sha256"))
      invalid("workload_input_mismatch", "power_overlay.source_sha256", "MNIST-12 overlay source bytes do not match the request.");
    const Config overlay_config = overlay_load.config;
    const auto& fields = overlay.at("fields").object();
    const std::map<std::string, double> expected_fields = {
      {"static_power_w",overlay_config.static_power_w}, {"npu_static_power_per_mac_w",overlay_config.npu_static_power_per_mac_w},
      {"rvv_static_power_per_lane_w",overlay_config.rvv_static_power_per_lane_w}, {"sram_static_power_per_kib_w",overlay_config.sram_static_power_per_kib_w},
      {"dram_interface_static_power_w",overlay_config.dram_interface_static_power_w}, {"it_power_budget_w",overlay_config.it_power_budget_w}};
    exact_keys(fields, {"static_power_w","npu_static_power_per_mac_w","rvv_static_power_per_lane_w","sram_static_power_per_kib_w","dram_interface_static_power_w","it_power_budget_w"}, "power_overlay.fields");
    for (const auto& [key, expected] : expected_fields) {
      if (request.overrides.contains(key)) invalid("workload_input_mismatch", "configuration.overrides." + key, "Study overlay fields must not be repeated as configuration overrides.");
      if (as_number(fields.at(key), "power_overlay.fields." + key) != expected)
        invalid("workload_input_mismatch", "power_overlay.fields." + key, "MNIST-12 overlay coefficient differs from its source manifest.");
    }
    config.static_power_w = expected_fields.at("static_power_w");
    config.npu_static_power_per_mac_w = expected_fields.at("npu_static_power_per_mac_w");
    config.rvv_static_power_per_lane_w = expected_fields.at("rvv_static_power_per_lane_w");
    config.sram_static_power_per_kib_w = expected_fields.at("sram_static_power_per_kib_w");
    config.dram_interface_static_power_w = expected_fields.at("dram_interface_static_power_w");
    config.it_power_budget_w = expected_fields.at("it_power_budget_w");
  } else {
    if (request.power_overlay.is_null()) invalid("workload_input_mismatch", "power_overlay", "Non-MNIST requests must identify their resolved config without applying an overlay.");
    const auto& overlay = request.power_overlay.object();
    if (!overlay.at("fields").object().empty()
        || as_string(overlay.at("source_path"), "power_overlay.source_path") != request.base_config_path
        || as_string(overlay.at("source_sha256"), "power_overlay.source_sha256") != request.base_config_sha256) {
      invalid("workload_input_mismatch", "power_overlay", "Non-MNIST workloads may identify their base config but cannot apply an overlay.");
    }
  }

  if (config.workload_mode != request.workload_mode)
    invalid("workload_input_mismatch", "workload.workload_mode", "Workload mode differs from the resolved configuration.");
  if (request.workload_mode == "physical_ai" && (config.workload_profile.size() > 0 || config.mapping_plan.size() > 0))
    invalid("workload_input_mismatch", "workload", "Physical AI v1 request cannot declare profile-only configuration inputs.");
  if (request.workload_mode == "profiled_graph" && config.workload_profile.empty())
    invalid("workload_input_mismatch", "workload.profile_path", "Profiled graph config does not resolve a workload profile.");
  const auto& workload = request.root.at("workload").object();
  const Json& declared_profile_path = workload.at("profile_path");
  const Json& declared_profile_hash = workload.at("profile_sha256");
  std::optional<std::string> profile_contents;
  if (config.workload_profile.empty()) {
    if (!declared_profile_path.is_null() || !declared_profile_hash.is_null()) invalid("workload_input_mismatch", "workload.profile_path", "Request declares a profile unused by the resolved configuration.");
  } else {
    const auto actual_path = repo_relative(root, config.workload_profile);
    if (declared_profile_path.is_null() || as_string(declared_profile_path, "workload.profile_path") != actual_path)
      invalid("workload_input_mismatch", "workload.profile_path", "Declared profile path differs from the resolved configuration input.");
    const auto profile_file = repo_file(root, actual_path, "workload.profile_path");
    profile_contents = read_file(profile_file);
    if (sha256(*profile_contents) != as_string(declared_profile_hash, "workload.profile_sha256"))
      invalid("workload_input_mismatch", "workload.profile_sha256", "Workload profile bytes do not match the request.");
  }
  const Json& declared_mapping_path = workload.at("mapping_path");
  const Json& declared_mapping_hash = workload.at("mapping_sha256");
  std::optional<std::string> mapping_contents;
  if (config.mapping_plan.empty()) {
    if (!declared_mapping_path.is_null() || !declared_mapping_hash.is_null()) invalid("workload_input_mismatch", "workload.mapping_path", "Request declares a mapping unused by the resolved configuration.");
  } else {
    const auto actual_path = repo_relative(root, config.mapping_plan);
    if (declared_mapping_path.is_null() || as_string(declared_mapping_path, "workload.mapping_path") != actual_path)
      invalid("workload_input_mismatch", "workload.mapping_path", "Declared mapping path differs from the resolved configuration input.");
    const auto mapping_file = repo_file(root, actual_path, "workload.mapping_path");
    mapping_contents = read_file(mapping_file);
    if (sha256(*mapping_contents) != as_string(declared_mapping_hash, "workload.mapping_sha256"))
      invalid("workload_input_mismatch", "workload.mapping_sha256", "Mapping plan bytes do not match the request.");
  }
  const auto& release = request.release_schedule;
  std::optional<std::string> release_trace_contents;
  if (release.at("kind").string() == "periodic") {
    if (config.frame_arrival_mode != "periodic" || as_number(release.at("period_us"), "workload.release_schedule.period_us") != config.frame_period_us)
      invalid("release_schedule_mismatch", "workload.release_schedule", "Periodic release schedule differs from the resolved configuration.");
  } else {
    const std::string declared_path = as_string(release.at("path"), "workload.release_schedule.path");
    if (config.frame_arrival_mode != "trace" || declared_path != repo_relative(root, config.frame_arrival_trace))
      invalid("release_schedule_mismatch", "workload.release_schedule", "Trace release schedule differs from the resolved configuration.");
    const auto trace_file = repo_file(root, declared_path, "workload.release_schedule.path");
    release_trace_contents = read_file(trace_file);
    if (sha256(*release_trace_contents) != as_string(release.at("sha256"), "workload.release_schedule.sha256"))
      invalid("release_schedule_mismatch", "workload.release_schedule.sha256", "Frame-arrival trace bytes do not match the request.");
  }
  const auto& deadlines = request.protocol.at("deadline_policy").object();
  const auto check_deadline = [&deadlines](const std::string& name, double resolved, bool enabled) {
    const auto& policy = deadlines.at(name).object();
    const Json& value = policy.at("relative_deadline_us");
    if (!enabled) {
      if (!value.is_null() || policy.at("handling").string() != "not_applicable") invalid("deadline_config_mismatch", "measurement_protocol.deadline_policy." + name, "Disabled control deadline must be null and not_applicable.");
    } else if (value.is_null() || as_number(value, "measurement_protocol.deadline_policy." + name + ".relative_deadline_us") != resolved
               || policy.at("handling").string() == "not_applicable") {
      invalid("deadline_config_mismatch", "measurement_protocol.deadline_policy." + name, "Deadline policy differs from resolved scheduler configuration.");
    }
  };
  check_deadline("frame", config.deadline_us, true);
  check_deadline("control", config.control_deadline_us, config.control_period_us > 0.0);
  validate(config,release_trace_contents);

  ResolvedCandidate resolved;
  resolved.config = config;
  resolved.root = canonical_root;
  resolved.effective_configuration = config_json(config, canonical_root);
  resolved.frame_arrivals_us = load_frame_arrivals(config,release_trace_contents);
  resolved.max_trace_bytes = as_uint(request.root.at("trace_policy").object().at("max_trace_bytes"), "trace_policy.max_trace_bytes");
  if (!config.workload_profile.empty()) {
    if (!profile_contents) invalid("workload_input_mismatch", "workload.profile_path", "Resolved profile bytes are unavailable.");
    resolved.profile = load_workload_profile_text(*profile_contents);
    if (!config.mapping_plan.empty()) {
      if (!mapping_contents) invalid("workload_input_mismatch", "workload.mapping_path", "Resolved mapping bytes are unavailable.");
      apply_mapping_plan(resolved.profile, load_mapping_plan_text(config.mapping_plan,resolved.profile,*mapping_contents), config);
    }
    resolved.has_profile = true;
  }
  resolved.power_coverage = build_power_coverage(request, config, resolved.has_profile ? &resolved.profile : nullptr);
  return resolved;
}

[[nodiscard]] Json count_record(std::uint64_t frame, std::uint64_t control) {
  return Json::Object{{"frame",Json(frame)}, {"control",Json(control)}, {"total",Json(frame + control)}};
}

[[nodiscard]] Json deadline_record(const StudyCohortMetrics& cohort, bool deadline_configured) {
  if (!deadline_configured) return Json::Object{{"released_count",Json(cohort.released)}, {"miss_count",Json(nullptr)},
      {"miss_rate",Json(nullptr)}, {"unavailable_reason",Json("deadline_not_configured")}};
  std::string reason;
  if (cohort.deadline_misses.value) reason.clear();
  else if (cohort.scheduled == 0) reason = "empty_target_cohort";
  else reason = "target_cohort_incomplete";
  return Json::Object{{"released_count",Json(cohort.released)},
      {"miss_count",cohort.deadline_misses.value ? Json(*cohort.deadline_misses.value) : Json(nullptr)},
      {"miss_rate",cohort.deadline_miss_rate.value ? Json(*cohort.deadline_miss_rate.value) : Json(nullptr)},
      {"unavailable_reason",reason.empty() ? Json(nullptr) : Json(reason)}};
}
[[nodiscard]] Json latency_record(const StudyCohortMetrics& cohort) {
  const auto reason_for = [&cohort](const StudyMetric<double>& metric) {
    if (metric.value) return Json(nullptr);
    return Json(cohort.scheduled == 0 ? "empty_target_cohort" : "target_cohort_incomplete");
  };
  const auto value_for = [](const StudyMetric<double>& metric) { return metric.value ? Json(*metric.value) : Json(nullptr); };
  const std::uint64_t samples = cohort.completed_by_drain;
  return Json::Object{{"sample_count",Json(samples)}, {"mean_us",value_for(cohort.mean_latency_us)},
      {"mean_unavailable_reason",reason_for(cohort.mean_latency_us)}, {"p99_us",value_for(cohort.p99_latency_us)},
      {"p99_unavailable_reason",reason_for(cohort.p99_latency_us)}};
}

[[nodiscard]] Json queue_counter(const std::optional<std::uint64_t>& count, const std::string& reason) {
  return Json::Object{{"count",count ? Json(*count) : Json(nullptr)}, {"unavailable_reason",count ? Json(nullptr) : Json(reason)}};
}
[[nodiscard]] Json queue_resource(const std::optional<StudyQueueSnapshot>& snapshot, std::size_t resource) {
  if (!snapshot) return Json::Object{{"ready",queue_counter(std::nullopt,"not_applicable")},
      {"in_flight",queue_counter(std::nullopt,"not_applicable")},
      {"dependency_blocked",queue_counter(std::nullopt,"not_applicable")}};
  const auto ready_count = snapshot->ready_by_resource[resource];
  const auto flight_count = snapshot->in_flight_by_resource[resource];
  return Json::Object{{"ready",queue_counter(ready_count,"")}, {"in_flight",queue_counter(flight_count,"")},
      {"dependency_blocked",queue_counter(std::nullopt,"not_materialized_by_workload_path")}};
}
[[nodiscard]] Json queue_state(const std::optional<StudyQueueSnapshot>& snapshot, bool profile_path) {
  (void)profile_path;
  return Json::Object{{"dma",queue_resource(snapshot,0)},
      {"rvv",queue_resource(snapshot,1)}, {"npu",queue_resource(snapshot,2)}};
}

[[nodiscard]] Json work_unit_measurements(const StudyMeasurementResult& result) {
  const auto frame_scheduled = result.frame_cohort.scheduled;
  const auto control_scheduled = result.control_cohort.scheduled;
  const auto frame_completed = result.frame_cohort.completed_by_drain;
  const auto control_completed = result.control_cohort.completed_by_drain;
  std::uint64_t released_before_frame = 0;
  std::uint64_t released_before_control = 0;
  if (result.mode == StudyMeasurementMode::Sustained) {
    for (const auto& unit : result.work_units) {
      if (unit.target_cohort || !unit.released) continue;
      if (unit.spec.kind == StudyWorkUnitKind::Frame) ++released_before_frame;
      else ++released_before_control;
    }
  }
  const std::uint64_t scheduled_frames = result.scheduled_frames;
  const std::uint64_t scheduled_controls = result.scheduled_controls;
  const std::uint64_t scheduled_before_frames = scheduled_frames;
  const std::uint64_t scheduled_before_controls = scheduled_controls;
  const std::uint64_t scheduled_unreleased_frames = scheduled_frames - result.released_frames;
  const std::uint64_t scheduled_unreleased_controls = scheduled_controls - result.released_controls;
  const std::uint64_t released_target_frames = result.frame_cohort.released;
  const std::uint64_t released_target_controls = result.control_cohort.released;
  const std::uint64_t scheduled_but_unreleased_frames = result.mode == StudyMeasurementMode::Sustained
      ? scheduled_frames - released_before_frame - released_target_frames : scheduled_unreleased_frames;
  const std::uint64_t scheduled_but_unreleased_controls = result.mode == StudyMeasurementMode::Sustained
      ? scheduled_controls - released_before_control - released_target_controls : scheduled_unreleased_controls;
  const std::uint64_t target_incomplete_frames = frame_scheduled - frame_completed;
  const std::uint64_t target_incomplete_controls = control_scheduled - control_completed;
  const std::uint64_t final_unfinished_frames = static_cast<std::uint64_t>(std::count_if(result.work_units.begin(), result.work_units.end(), [](const StudyWorkUnitRecord& u) { return u.spec.kind == StudyWorkUnitKind::Frame && u.released && !u.completion_us; }));
  const std::uint64_t final_unfinished_controls = static_cast<std::uint64_t>(std::count_if(result.work_units.begin(), result.work_units.end(), [](const StudyWorkUnitRecord& u) { return u.spec.kind == StudyWorkUnitKind::Control && u.released && !u.completion_us; }));
  const std::uint64_t completed_window_frames = result.completed_frames_in_window;
  const std::uint64_t completed_window_controls = result.completed_controls_in_window;
  return Json::Object{
    {"scheduled_before_cutoff",count_record(scheduled_before_frames,scheduled_before_controls)},
    {"scheduled_target_cohort",count_record(frame_scheduled,control_scheduled)},
    {"released_before_window",count_record(released_before_frame,released_before_control)},
    {"released_target_cohort",count_record(released_target_frames,released_target_controls)},
    {"completed_in_window",count_record(completed_window_frames,completed_window_controls)},
    {"completed_by_drain",count_record(frame_completed,control_completed)},
    {"incomplete_target_cohort",count_record(target_incomplete_frames,target_incomplete_controls)},
    {"scheduled_but_unreleased",count_record(scheduled_but_unreleased_frames,scheduled_but_unreleased_controls)},
    {"final_unfinished",count_record(final_unfinished_frames,final_unfinished_controls)}};
}

[[nodiscard]] Json offered_work(const StudyMeasurementResult& result) {
  if (result.mode == StudyMeasurementMode::Finite) return Json::Object{{"frame",Json(nullptr)}, {"control",Json(nullptr)},
      {"total",Json(nullptr)}, {"unavailable_reason",Json("not_applicable_finite_protocol")}};
  const auto total = result.offered_throughput_per_s.value;
  return Json::Object{{"frame",result.frame_cohort.offered_rate_per_s.value ? Json(*result.frame_cohort.offered_rate_per_s.value) : Json(nullptr)},
      {"control",result.control_cohort.offered_rate_per_s.value ? Json(*result.control_cohort.offered_rate_per_s.value) : Json(nullptr)},
      {"total",total ? Json(*total) : Json(nullptr)}, {"unavailable_reason",total ? Json(nullptr) : Json("measurement_window_incomplete")}};
}

[[nodiscard]] Json measurements_json(const StudyMeasurementResult& r, bool controls_enabled, bool profile_path) {
  const auto& e = r.energy;
  const bool energy_available = e.it_energy_j.value.has_value();
  const std::string energy_reason = energy_available ? "" : (e.it_energy_j.null_reason.empty() ? "measurement_window_incomplete" : e.it_energy_j.null_reason);
  const auto energy_value = [](const StudyMetric<double>& metric) -> Json {
    if (metric.value) return Json(*metric.value);
    return Json(nullptr);
  };
  const Json energy = Json::Object{
    {"dynamic_by_category_j",Json::Object{{"vector",energy_value(e.vector_energy_j)}, {"npu_arithmetic",energy_value(e.npu_arithmetic_energy_j)},
       {"dram",energy_value(e.dram_energy_j)}, {"dma",energy_value(e.dma_energy_j)}}},
    {"static_j",energy_value(e.static_energy_j)}, {"total_it_j",energy_value(e.it_energy_j)},
    {"average_it_power_w",energy_value(e.average_it_power_w)},
    {"energy_per_completed_work_unit_j",energy_value(e.energy_per_completed_work_unit_j)},
    {"energy_work_unit_denominator",Json(r.mode == StudyMeasurementMode::Finite ? (r.completed_frames_by_drain + r.completed_controls_by_drain) : (r.completed_frames_in_window + r.completed_controls_in_window))},
    {"unavailable_reason",energy_available ? Json(nullptr) : Json(energy_reason)},
    {"energy_per_completed_work_unit_unavailable_reason",e.energy_per_completed_work_unit_j.value ? Json(nullptr) : Json(e.energy_per_completed_work_unit_j.null_reason.empty() ? "no_completed_work_units" : e.energy_per_completed_work_unit_j.null_reason)}};
  Json::Object measurements{
    {"interval",Json::Object{{"start_us",Json(r.interval_start_us)}, {"end_us",Json(r.interval_end_us)}, {"duration_us",Json(r.interval_duration_us)}, {"complete",Json(r.interval_complete)}}},
    {"offered_work_units_per_second",offered_work(r)},
    {"work_units",work_unit_measurements(r)},
    {"throughput_work_units_per_second",r.completed_throughput_per_s.value ? Json(*r.completed_throughput_per_s.value) : Json(nullptr)},
    {"throughput_unavailable_reason",r.completed_throughput_per_s.value ? Json(nullptr) : Json(r.completed_throughput_per_s.null_reason)},
    {"latency",Json::Object{{"frame",latency_record(r.frame_cohort)}, {"control",latency_record(r.control_cohort)}}},
    {"deadline",Json::Object{{"frame",deadline_record(r.frame_cohort,true)}, {"control",deadline_record(r.control_cohort,controls_enabled)}}},
    {"backlog",Json::Object{{"at_window_start",r.mode == StudyMeasurementMode::Sustained && r.start_snapshot ? Json(r.start_snapshot->released_unfinished_work_units) : Json(nullptr)},
       {"at_window_end",r.mode == StudyMeasurementMode::Sustained && r.end_snapshot ? Json(r.end_snapshot->released_unfinished_work_units) : Json(nullptr)},
       {"delta",r.backlog_delta_work_units ? Json(*r.backlog_delta_work_units) : Json(nullptr)},
       {"unavailable_reason",r.mode == StudyMeasurementMode::Finite ? Json("not_applicable") : ((!r.start_snapshot || !r.end_snapshot) ? Json("measurement_window_incomplete") : Json(nullptr))}}},
    {"queue_state_at_window_start",queue_state(r.mode == StudyMeasurementMode::Sustained ? r.start_snapshot : std::nullopt,profile_path)},
    {"queue_state_at_window_end",queue_state(r.mode == StudyMeasurementMode::Sustained ? r.end_snapshot : std::nullopt,profile_path)},
    {"last_model_time_us",Json(r.last_model_time_us)},
    {"energy",energy}};
  return Json(std::move(measurements));
}

[[nodiscard]] Json::Object request_id_echo(const Json& parsed_json) {
  Json::Object identities;
  static const std::array<std::string,7> names = {"candidate_id","manifest_id","simulator_build_identity","configuration_identity","workload_identity","model_assumption_identity","measurement_protocol_identity"};
  for (const auto& name : names) {
    Json value = nullptr;
    if (parsed_json.is_object()) {
      const auto found = parsed_json.object().find(name);
      if (found != parsed_json.object().end() && found->second.is_string() && is_sha256(found->second.string())) value = found->second;
    }
    identities.emplace(name,std::move(value));
  }
  return identities;
}

[[nodiscard]] std::string bounded_field(std::string field) {
  if (field.size() > 128) field.resize(128);
  if (!std::all_of(field.begin(), field.end(), [](unsigned char c) { return c <= 0x7f; })) return "";
  return field;
}
[[nodiscard]] std::string stable_message(const std::string& code) {
  static const std::map<std::string,std::string> messages = {
    {"invalid_json","Request is not valid JSON."}, {"unsupported_schema","Request schema is unsupported."},
    {"missing_field","A required request field is missing."}, {"unknown_field","Request contains an unsupported field."},
    {"invalid_enum","Request contains an unsupported enum value."}, {"invalid_type","Request field has an invalid type or value."},
    {"identity_mismatch","A declared identity does not match the evaluated input."},
    {"config_error","Configuration could not be resolved or validated."},
    {"configuration_source_mismatch","Loaded configuration sources do not match the request."},
    {"workload_input_mismatch","Resolved workload inputs do not match the request."},
    {"release_schedule_mismatch","Release schedule does not match the resolved configuration."},
    {"deadline_config_mismatch","Deadline policy differs from resolved scheduler configuration."},
    {"empty_workload_unsupported","Candidate requires at least one scheduled work unit."},
    {"empty_target_cohort_unsupported","Sustained candidate requires a non-empty target cohort."},
    {"study_budget_mismatch","Study candidate must use the exact 0.23 W modeled IT-power budget."},
    {"candidate_result_limit","Candidate result exceeded its declared byte limit."},
    {"simulation_time_limit","Candidate reached its simulated-time limit."}, {"drain_time_limit","Candidate reached its cohort-drain limit."},
    {"task_event_limit","Candidate reached its task-event limit."}, {"backlog_limit","Candidate reached its backlog limit."},
    {"simulation_failure","Candidate simulation failed."}};
  const auto found = messages.find(code);
  return found == messages.end() ? "Candidate evaluation failed." : found->second;
}

[[nodiscard]] Json error_record(std::string code, std::string field = "") {
  std::string message = stable_message(code);
  if (message.size() > 256) message.resize(256);
  return Json::Object{{"code",Json(code)}, {"message",Json(message)},
      {"field",field.empty() ? Json(nullptr) : Json(bounded_field(std::move(field)))}};
}

[[nodiscard]] Json trace_export_record(const std::string& status, std::uint64_t bytes = 0,
                                       std::uint64_t records = 0, std::optional<std::string> digest = std::nullopt,
                                       std::optional<std::string> reason = std::nullopt) {
  return Json::Object{{"status",Json(status)}, {"bytes",Json(bytes)}, {"record_count",Json(records)},
      {"sha256",digest ? Json(*digest) : Json(nullptr)}, {"unavailable_reason",reason ? Json(*reason) : Json(nullptr)}};
}

[[nodiscard]] Json::Object base_result(const Json::Object& echoes) {
  Json::Object result;
  result.emplace("schema_id",Json("axissim.study-candidate-result.v1"));
  for (const auto& [key,value] : echoes) result.emplace(key,value);
  result.emplace("measurement_protocol",Json(nullptr));
  result.emplace("measurement_protocol_unavailable_reason",Json("invalid_or_unparsed_request"));
  result.emplace("effective_configuration",Json::Object{});
  result.emplace("effective_configuration_unavailable_reason",Json("configuration_not_resolved"));
  result.emplace("execution_status",Json("invalid"));
  result.emplace("power_budget_status",Json("not_evaluated"));
  result.emplace("configured_it_power_budget_w",Json(nullptr));
  result.emplace("configured_it_power_budget_unavailable_reason",Json("configuration_not_resolved"));
  result.emplace("feasibility_label",Json(nullptr));
  result.emplace("operating_state",Json("unknown"));
  result.emplace("measurements",Json(nullptr));
  result.emplace("measurements_unavailable_reason",Json("invalid_input"));
  result.emplace("power_accounting_coverage",Json(nullptr));
  result.emplace("power_accounting_coverage_unavailable_reason",Json("invalid_or_unparsed_request"));
  result.emplace("trace_export",trace_export_record("not_requested"));
  result.emplace("censor_reason",Json(nullptr));
  result.emplace("errors",Json::Array{});
  return result;
}

void attach_error(Json::Object& result, const std::string& code, const std::string& field = "") {
  result["errors"] = Json::Array{error_record(code,field)};
}

void append_trace_field(std::string& line, std::string_view key, const Json& value, bool& first) {
  if (!first) line.push_back(',');
  first = false;
  append_escaped(line,key); line.push_back(':'); serialize_into(line,value);
}
[[nodiscard]] std::string trace_metadata_line(const CandidateRequest& request) {
  const auto ids = request_id_echo(Json(request.root));
  std::string line = "{";
  bool first = true;
  append_trace_field(line,"schema_id",Json("axissim.study-task-trace.v1"),first);
  for (const auto& name : {"candidate_id","manifest_id","simulator_build_identity","configuration_identity","workload_identity","model_assumption_identity","measurement_protocol_identity"})
    append_trace_field(line,name,ids.at(name),first);
  append_trace_field(line,"time_unit",Json("us"),first);
  append_trace_field(line,"energy_unit",Json("J"),first);
  append_trace_field(line,"activity_assumption",Json("uniform_over_task_interval"),first);
  line += "}\n";
  return line;
}
[[nodiscard]] std::string trace_task_line(const StudyTaskIntervalRecord& task) {
  std::string resource = task.resource == "DMA+DRAM" ? "dma" : task.resource == "RVV" ? "rvv" : "npu";
  std::string line = "{";
  bool first = true;
  append_trace_field(line,"record_type",Json("task"),first);
  append_trace_field(line,"task_sequence",Json(task.task_id),first);
  append_trace_field(line,"work_unit_id",Json(task.work_unit_id),first);
  append_trace_field(line,"task_name",Json(task.task_name),first);
  append_trace_field(line,"resource",Json(resource),first);
  append_trace_field(line,"release_us",Json(task.release_us),first);
  append_trace_field(line,"ready_us",Json(task.ready_us),first);
  append_trace_field(line,"start_us",Json(task.start_us),first);
  append_trace_field(line,"scheduled_end_us",Json(task.end_us),first);
  append_trace_field(line,"observed_end_us",Json(task.observed_end_us),first);
  append_trace_field(line,"completion_status",Json(task.completion_status),first);
  if (!first) line.push_back(',');
  first = false;
  append_escaped(line,"energy_j"); line += ":{";
  bool energy_first = true;
  for (const auto& [key,value] : std::array<std::pair<std::string_view,double>,4>{{
         {"vector",task.vector_energy_j},{"npu_arithmetic",task.npu_arithmetic_energy_j},
         {"dram",task.dram_energy_j},{"dma",task.dma_energy_j}}}) {
    if (!energy_first) line.push_back(',');
    energy_first = false;
    append_escaped(line,key); line.push_back(':'); serialize_into(line,Json(value));
  }
  line.push_back('}');
  line += "}\n";
  return line;
}

struct TraceOutcome {
  Json record = trace_export_record("not_requested");
  std::string status = "not_requested";
};
[[nodiscard]] TraceOutcome write_study_trace(const std::string& path, const CandidateRequest& request,
                                             const StudyMeasurementResult& measurement, std::uint64_t cap) {
  const std::filesystem::path final_path(path);
  const std::filesystem::path temp_path = final_path.string() + ".tmp";
  std::error_code ec;
  std::filesystem::remove(temp_path,ec);
  std::ofstream out(temp_path,std::ios::binary);
  if (!out) return {trace_export_record("failed",0,0,std::nullopt,"trace_io_error"),"failed"};
  const std::string header = trace_metadata_line(request);
  std::uint64_t bytes = 0;
  std::uint64_t records = 0;
  const auto append = [&out,&bytes,cap](const std::string& line) {
    if (line.size() > cap - std::min(cap,bytes)) return false;
    out.write(line.data(),static_cast<std::streamsize>(line.size()));
    if (!out) throw std::runtime_error("trace write");
    bytes += static_cast<std::uint64_t>(line.size());
    return true;
  };
  try {
    if (!append(header)) { out.close(); std::filesystem::remove(temp_path,ec); return {trace_export_record("omitted_limit",0,0,std::nullopt,"max_trace_bytes"),"omitted_limit"}; }
    for (const auto& task : measurement.task_intervals) {
      const std::string line = trace_task_line(task);
      if (!append(line)) { out.close(); std::filesystem::remove(temp_path,ec); return {trace_export_record("omitted_limit",0,0,std::nullopt,"max_trace_bytes"),"omitted_limit"}; }
      ++records;
    }
    out.close();
    if (!out) throw std::runtime_error("trace close");
    std::filesystem::rename(temp_path,final_path,ec);
    if (ec) throw std::runtime_error("trace rename");
    const auto digest = sha256_file(final_path);
    return {trace_export_record("written",bytes,records,digest),"written"};
  } catch (...) {
    out.close(); std::filesystem::remove(temp_path,ec);
    return {trace_export_record("failed",0,0,std::nullopt,"trace_io_error"),"failed"};
  }
}

void write_atomic(const std::string& path, const std::string& content) {
  const std::filesystem::path final_path(path);
  const auto parent = final_path.parent_path();
  if (!parent.empty()) std::filesystem::create_directories(parent);
  const std::filesystem::path temp_path = final_path.string() + ".tmp";
  {
    std::ofstream output(temp_path,std::ios::binary|std::ios::trunc);
    if (!output) throw std::runtime_error("result_open");
    output.write(content.data(),static_cast<std::streamsize>(content.size()));
    output.close();
    if (!output) throw std::runtime_error("result_write");
  }
  std::error_code ec;
  std::filesystem::rename(temp_path,final_path,ec);
  if (ec) { std::filesystem::remove(temp_path); throw std::runtime_error("result_rename"); }
}

[[nodiscard]] std::string execution_name(StudyExecutionStatus status) {
  switch (status) {
    case StudyExecutionStatus::Complete: return "completed";
    case StudyExecutionStatus::Invalid: return "invalid";
    case StudyExecutionStatus::Censored: return "censored";
    case StudyExecutionStatus::Failed: return "failed";
    case StudyExecutionStatus::Running: return "failed";
  }
  return "failed";
}
[[nodiscard]] std::string censor_name(StudyCensorReason reason) {
  switch (reason) {
    case StudyCensorReason::MaxSimulatedTime: return "simulation_time_limit";
    case StudyCensorReason::MaxDrainSimulatedTime: return "drain_time_limit";
    case StudyCensorReason::MaxTaskEvents: return "task_event_limit";
    case StudyCensorReason::MaxBacklogWorkUnits: return "backlog_limit";
    case StudyCensorReason::None: return "";
  }
  return "";
}

[[nodiscard]] Json valid_result(const CandidateRequest& request, const ResolvedCandidate& resolved,
                                const StudyMeasurementResult* measurement, const TraceOutcome& trace,
                                const std::string& status, const std::string& error_code = "",
                                const std::string& error_field = "") {
  Json::Object result = base_result(request_id_echo(Json(request.root)));
  result["measurement_protocol"] = Json(request.protocol);
  result["measurement_protocol_unavailable_reason"] = Json(nullptr);
  result["effective_configuration"] = resolved.effective_configuration;
  result["effective_configuration_unavailable_reason"] = Json(nullptr);
  result["execution_status"] = Json(status);
  result["configured_it_power_budget_w"] = Json(resolved.config.it_power_budget_w);
  result["configured_it_power_budget_unavailable_reason"] = Json(nullptr);
  result["power_accounting_coverage"] = resolved.power_coverage;
  result["power_accounting_coverage_unavailable_reason"] = Json(nullptr);
  result["trace_export"] = trace.record;
  if (request.extensions_present) result["extensions"] = Json(request.extensions);
  if (measurement != nullptr) {
    if (status == "invalid" || status == "failed") {
      result["measurements"] = Json(nullptr);
      result["measurements_unavailable_reason"] = Json(status == "invalid" ? "invalid_input" : "runtime_failure");
    } else {
      result["measurements"] = measurements_json(*measurement,resolved.config.control_period_us > 0.0,resolved.has_profile);
      result["measurements_unavailable_reason"] = Json(nullptr);
    }
    result["operating_state"] = Json(measurement->operating_state);
    if (measurement->power_budget_feasible.has_value()) {
      const bool pass = *measurement->power_budget_feasible;
      result["power_budget_status"] = Json(pass ? "pass" : "fail");
      result["feasibility_label"] = pass ? Json("Provisionally feasible under current modeled power terms.") : Json(nullptr);
    }
    const std::string censor = censor_name(measurement->censor_reason);
    if (!censor.empty()) {
      result["censor_reason"] = Json(censor);
      attach_error(result,censor);
    }
  }
  if (!error_code.empty()) attach_error(result,error_code,error_field);
  return Json(std::move(result));
}

[[nodiscard]] Json invalid_result(const Json& parsed_json, const ContractError& error) {
  auto result = base_result(request_id_echo(parsed_json));
  attach_error(result,error.code,error.field);
  return Json(std::move(result));
}

[[nodiscard]] Json runtime_failure_result(const CandidateRequest& request, const ResolvedCandidate& resolved,
                                         const std::string& code, const TraceOutcome& trace) {
  auto result = base_result(request_id_echo(Json(request.root)));
  result["measurement_protocol"] = Json(request.protocol);
  result["measurement_protocol_unavailable_reason"] = Json(nullptr);
  result["effective_configuration"] = resolved.effective_configuration;
  result["effective_configuration_unavailable_reason"] = Json(nullptr);
  result["execution_status"] = Json("failed");
  result["configured_it_power_budget_w"] = Json(resolved.config.it_power_budget_w);
  result["configured_it_power_budget_unavailable_reason"] = Json(nullptr);
  result["power_accounting_coverage"] = resolved.power_coverage;
  result["power_accounting_coverage_unavailable_reason"] = Json(nullptr);
  result["trace_export"] = trace.record;
  if (request.extensions_present) result["extensions"] = Json(request.extensions);
  attach_error(result,code);
  result["measurements_unavailable_reason"] = Json("runtime_failure");
  return Json(std::move(result));
}

[[nodiscard]] Json result_limit_fallback(const CandidateRequest& request, const ResolvedCandidate& resolved) {
  auto result = base_result(request_id_echo(Json(request.root)));
  result["measurement_protocol"] = Json(request.protocol);
  result["measurement_protocol_unavailable_reason"] = Json(nullptr);
  result["effective_configuration"] = Json::Object{};
  result["effective_configuration_unavailable_reason"] = Json("candidate_result_limit");
  result["execution_status"] = Json("failed");
  result["configured_it_power_budget_w"] = Json(resolved.config.it_power_budget_w);
  result["configured_it_power_budget_unavailable_reason"] = Json(nullptr);
  result["power_accounting_coverage"] = Json(nullptr);
  result["power_accounting_coverage_unavailable_reason"] = Json("candidate_result_limit");
  result["trace_export"] = trace_export_record("omitted_limit",0,0,std::nullopt,"candidate_result_limit");
  result["measurements_unavailable_reason"] = Json("candidate_result_limit");
  attach_error(result,"candidate_result_limit");
  return Json(std::move(result));
}

}  // namespace

int run_study_evaluate(const std::string& request_path, const std::string& result_path,
                       const std::string& executable_path, const std::optional<std::string>& trace_path) {
  Json parsed_json;
  try {
    parsed_json = JsonParser(read_file(request_path)).parse();
  } catch (const ContractError& error) {
    const auto output = serialize(invalid_result(parsed_json,error)) + "\n";
    write_atomic(result_path,output);
    return 2;
  } catch (...) {
    ContractError error("invalid_json","",stable_message("invalid_json"));
    const auto output = serialize(invalid_result(parsed_json,error)) + "\n";
    write_atomic(result_path,output);
    return 2;
  }

  CandidateRequest request;
  try {
    request = parse_request(parsed_json);
  } catch (const ContractError& error) {
    write_atomic(result_path,serialize(invalid_result(parsed_json,error))+"\n");
    return 2;
  }
  if (request.retain_trace != trace_path.has_value()) {
    ContractError error("invalid_type","trace_policy.retain_full_trace","Trace CLI option must match the request trace policy.");
    write_atomic(result_path,serialize(invalid_result(parsed_json,error))+"\n");
    return 2;
  }

  ResolvedCandidate resolved;
  try {
    resolved = resolve_candidate(request,std::filesystem::current_path(),executable_path);
  } catch (const ContractError& error) {
    write_atomic(result_path,serialize(invalid_result(parsed_json,error))+"\n");
    return 2;
  } catch (...) {
    ContractError error("config_error","configuration","Configuration or workload inputs could not be resolved.");
    write_atomic(result_path,serialize(invalid_result(parsed_json,error))+"\n");
    return 2;
  }

  if (resolved.config.it_power_budget_w != 0.23) {
    const Json result = valid_result(request,resolved,nullptr,TraceOutcome{trace_export_record("not_requested"),"not_requested"},
        "invalid","study_budget_mismatch","configuration.expected_it_power_budget_w");
    write_atomic(result_path,serialize(result)+"\n");
    return 2;
  }

  StudyMeasurementResult measurement;
  try {
    Simulator simulator(resolved.config);
    measurement = simulator.run_study(request.observer_request,
        resolved.has_profile ? &resolved.profile : nullptr, &resolved.frame_arrivals_us);
  } catch (...) {
    TraceOutcome trace{trace_export_record("not_requested"),"not_requested"};
    if (request.retain_trace) trace = {trace_export_record("failed",0,0,std::nullopt,"trace_io_error"),"failed"};
    Json result = runtime_failure_result(request,resolved,"simulation_failure",trace);
    std::string bytes = serialize(result)+"\n";
    if (bytes.size() > request.max_candidate_result_bytes) result = result_limit_fallback(request,resolved), bytes = serialize(result)+"\n";
    if (bytes.size() <= request.max_candidate_result_bytes) write_atomic(result_path,bytes);
    return 1;
  }

  const std::string status = execution_name(measurement.execution_status);
  TraceOutcome trace{trace_export_record("not_requested"),"not_requested"};
  if (request.retain_trace && status != "invalid" && status != "failed") {
    trace = write_study_trace(*trace_path,request,measurement,resolved.max_trace_bytes);
  }
  std::string error_code;
  if (status == "invalid") error_code = measurement.error_code == "empty_target_cohort_unsupported" ? "empty_target_cohort_unsupported" : "empty_workload_unsupported";
  else if (status == "failed") error_code = "simulation_failure";
  Json result = valid_result(request,resolved,&measurement,trace,status,error_code);
  std::string output = serialize(result)+"\n";
  bool result_size_limited = false;
  if (output.size() > request.max_candidate_result_bytes) {
    if (request.retain_trace && trace.status == "written") { std::error_code ec; std::filesystem::remove(*trace_path,ec); }
    result = result_limit_fallback(request,resolved);
    output = serialize(result)+"\n";
    result_size_limited = true;
  }
  if (output.size() > request.max_candidate_result_bytes) return 1;
  write_atomic(result_path,output);
  if (result_size_limited) return 1;
  return status == "completed" ? 0 : status == "invalid" ? 2 : status == "censored" ? 1 : 1;
}

}  // namespace axissim






