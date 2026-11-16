#include "gemm_json.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {
void require(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}
template <typename Fn>
void require_error(Fn &&fn, const std::string &code,
                   const std::string &path = "") {
  try {
    fn();
  } catch (const axissim::gemm_json::Error &error) {
    require(error.code == code, "unexpected JSON error code");
    require(error.path == path, "unexpected JSON error path");
    return;
  }
  require(false, "expected JSON parser error");
}
} // namespace

int main() {
  using namespace axissim::gemm_json;
  const auto value = parse(" { \"z\": [true, 2], \"a\": \"x\\n\" } ");
  require(canonical(value) == "{\"a\":\"x\\u000a\",\"z\":[true,2]}",
          "canonical ordering or escaping mismatch");
  require_error([] { (void)parse("{\"a\":1,\"a\":2}"); }, "duplicate_key",
                "/a");
  require_error([] { (void)parse("{\"z\":0,\"z\":1,\"a\":0,\"a\":1}"); },
                "duplicate_key", "/a");
  require_error([] { (void)parse("{bad}"); }, "invalid_json");
  require_error([] { (void)parse("\"\\ud800\""); }, "invalid_json");
  const std::string invalid_utf8 = std::string("\"") + "\xff" + "\"";
  require_error([&] { (void)parse(invalid_utf8); }, "invalid_json");
  require_error([] { (void)parse("NaN"); }, "invalid_json");
  require_error([] { (void)canonical(parse("-1")); }, "invalid_value");
  require_error([] { (void)canonical(parse("1.0")); }, "invalid_value");
  const std::string non_ascii = std::string("\"") + "\xc3\xa9" + "\"";
  require_error([&] { (void)canonical(parse(non_ascii)); }, "invalid_value");
  require(pointer_child("/a", "x/y~z") == "/a/x~1y~0z",
          "JSON pointer escaping mismatch");
  return 0;
}
