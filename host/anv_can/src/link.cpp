#include "anv/link.hpp"

#include <cctype>
#include <cstdio>
#include <cstring>

namespace anv {

Frame Frame::make(uint32_t id, const uint8_t* bytes, uint8_t n, bool extended) {
  Frame f;
  f.id = id & (extended ? kExtIdMask : kStdIdMask);
  f.extended = extended;
  f.len = n > 8 ? 8 : n;
  if (bytes != nullptr) std::memcpy(f.data.data(), bytes, f.len);
  return f;
}

std::string to_cansend(const Frame& f) {
  char buf[32];
  std::snprintf(buf, sizeof buf, f.extended ? "%08X#" : "%03X#", static_cast<unsigned>(f.id));
  std::string out = buf;
  for (uint8_t i = 0; i < f.len; ++i) {
    std::snprintf(buf, sizeof buf, "%02X", f.data[i]);
    out += buf;
  }
  return out;
}

namespace {

int hex_digit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

bool fail(std::string* error, const char* why) {
  if (error) *error = why;
  return false;
}

}  // namespace

bool from_cansend(const std::string& text, Frame& out, std::string* error) {
  const size_t hash = text.find('#');
  if (hash == std::string::npos) return fail(error, "expected ID#DATA");
  const std::string id_text = text.substr(0, hash);
  if (id_text.empty() || id_text.size() > 8) return fail(error, "bad ID");
  uint32_t id = 0;
  for (char c : id_text) {
    const int d = hex_digit(c);
    if (d < 0) return fail(error, "bad ID");
    id = (id << 4) | static_cast<uint32_t>(d);
  }
  const bool extended = id_text.size() > 3;
  if (extended && id > kExtIdMask) return fail(error, "ID over 29 bits");
  std::string data;
  for (size_t i = hash + 1; i < text.size(); ++i) {
    if (text[i] != '.') data.push_back(text[i]);  // cansend allows 11.22.33
  }
  if (data.size() % 2 != 0 || data.size() > 16) return fail(error, "data must be 0-8 hex bytes");
  uint8_t bytes[8];
  for (size_t i = 0; i < data.size() / 2; ++i) {
    const int hi = hex_digit(data[2 * i]), lo = hex_digit(data[2 * i + 1]);
    if (hi < 0 || lo < 0) return fail(error, "bad data");
    bytes[i] = static_cast<uint8_t>(hi << 4 | lo);
  }
  out = Frame::make(id, bytes, static_cast<uint8_t>(data.size() / 2), extended);
  return true;
}

}  // namespace anv
