#include "sha256.hpp"

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
} // namespace

int main() {
  require(
      axissim::sha256_hex("") ==
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
      "SHA-256 empty-input known vector mismatch");
  require(
      axissim::sha256_hex("abc") ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
      "SHA-256 abc known vector mismatch");
  return 0;
}
