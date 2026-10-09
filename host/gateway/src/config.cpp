#include "anv_gateway/config.hpp"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace anv::gw {

namespace {

std::string trim(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
  while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
  return s.substr(a, b - a);
}

bool parse_bool(const std::string& v, bool& out) {
  std::string l;
  for (char c : v) l.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  if (l == "true" || l == "yes" || l == "on" || l == "1") { out = true; return true; }
  if (l == "false" || l == "no" || l == "off" || l == "0") { out = false; return true; }
  return false;
}

bool parse_int(const std::string& v, long& out) {
  if (v.empty()) return false;
  char* end = nullptr;
  out = std::strtol(v.c_str(), &end, 10);
  return end != nullptr && *end == '\0';
}

bool fail(std::string* error, int line, const std::string& msg) {
  if (error) *error = "line " + std::to_string(line) + ": " + msg;
  return false;
}

}  // namespace

bool parse_config_text(const std::string& text, GatewayConfig& out, std::string* error) {
  GatewayConfig cfg = out;
  std::istringstream in(text);
  std::string raw;
  int line_no = 0;
  while (std::getline(in, raw)) {
    ++line_no;
    const size_t hash = raw.find('#');
    const std::string line = trim(hash == std::string::npos ? raw : raw.substr(0, hash));
    if (line.empty()) continue;
    const size_t colon = line.find(':');
    if (colon == std::string::npos) return fail(error, line_no, "expected 'key: value'");
    const std::string key = trim(line.substr(0, colon));
    std::string value = trim(line.substr(colon + 1));
    if (value.size() >= 2 && ((value.front() == '"' && value.back() == '"') ||
                              (value.front() == '\'' && value.back() == '\''))) {
      value = value.substr(1, value.size() - 2);
    }

    long n = 0;
    if (key == "interface") {
      if (value.empty()) return fail(error, line_no, "interface must not be empty");
      cfg.interface_name = value;
    } else if (key == "bridge_socket") {
      cfg.bridge_socket = value;  // empty value = off
    } else if (key == "host_id") {
      if (!parse_int(value, n) || (n != 1 && n != 2)) return fail(error, line_no, "host_id must be 1 (Orin) or 2 (RPi)");
      cfg.host_id = static_cast<uint8_t>(n);
    } else if (key == "tie_break") {
      if (!parse_int(value, n) || n < 1 || n > 255) return fail(error, line_no, "tie_break must be 1-255");
      cfg.tie_break = static_cast<uint8_t>(n);
    } else if (key == "auto_takeover") {
      if (!parse_bool(value, cfg.auto_takeover)) return fail(error, line_no, "auto_takeover must be true or false");
    } else if (key == "rt_priority") {
      if (!parse_int(value, n) || n < 0 || n > 99) return fail(error, line_no, "rt_priority must be 0-99");
      cfg.rt_priority = static_cast<int>(n);
    } else if (key == "cpu_affinity") {
      if (!parse_int(value, n) || n < -1 || n > 1023) return fail(error, line_no, "cpu_affinity must be -1 or a core number");
      cfg.cpu_affinity = static_cast<int>(n);
    } else if (key == "flight_recorder") {
      if (!parse_bool(value, cfg.flight_recorder)) return fail(error, line_no, "flight_recorder must be true or false");
    } else if (key == "flight_recorder_dir") {
      cfg.flight_recorder_dir = value;
    } else if (key == "event_log_dir") {
      cfg.event_log_dir = value;
    } else if (key == "link_enabled") {
      if (!parse_bool(value, cfg.link_enabled)) return fail(error, line_no, "link_enabled must be true or false");
    } else if (key == "link_listen_port") {
      if (!parse_int(value, n) || n < 1 || n > 65535) return fail(error, line_no, "link_listen_port must be 1-65535");
      cfg.link_listen_port = static_cast<uint16_t>(n);
    } else if (key == "link_peer_host") {
      cfg.link_peer_host = value;
    } else if (key == "link_peer_port") {
      if (!parse_int(value, n) || n < 1 || n > 65535) return fail(error, line_no, "link_peer_port must be 1-65535");
      cfg.link_peer_port = static_cast<uint16_t>(n);
    } else {
      return fail(error, line_no, "unknown key '" + key + "'");
    }
  }
  out = cfg;
  return true;
}

bool load_config_file(const std::string& path, GatewayConfig& out, std::string* error) {
  std::ifstream f(path);
  if (!f) {
    if (error) *error = "cannot open " + path;
    return false;
  }
  std::stringstream ss;
  ss << f.rdbuf();
  return parse_config_text(ss.str(), out, error);
}

}  // namespace anv::gw
