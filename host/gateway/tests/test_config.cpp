#include "anv_gateway/config.hpp"
#include "test_util.hpp"

using namespace anv::gw;

TEST(parses_all_keys_comments_and_quotes) {
  GatewayConfig c;
  std::string err;
  const std::string text =
      "# a comment\n"
      "interface: \"vcan0\"   # trailing comment\n"
      "host_id: 2\n"
      "tie_break: 2\n"
      "\n"
      "auto_takeover: yes\n"
      "rt_priority: 40\n"
      "cpu_affinity: 3\n";
  CHECK(parse_config_text(text, c, &err));
  CHECK(c.interface_name == "vcan0");
  CHECK_EQ(c.host_id, 2);
  CHECK_EQ(c.tie_break, 2);
  CHECK(c.auto_takeover);
  CHECK_EQ(c.rt_priority, 40);
  CHECK_EQ(c.cpu_affinity, 3);
}

TEST(defaults_are_safe) {
  GatewayConfig c;
  CHECK(!c.auto_takeover);  // auto-takeover is off unless asked for
  CHECK(c.interface_name == "can0");
  GatewayConfig rpi;
  rpi.host_id = 2;
  CHECK_EQ(rpi.effective_tie_break(), 2);  // tie_break falls back to host_id
}

TEST(rejects_bad_input_with_a_line_number) {
  GatewayConfig c;
  std::string err;
  CHECK(!parse_config_text("host_id: 3\n", c, &err));
  CHECK(err.find("line 1") != std::string::npos);
  CHECK(!parse_config_text("interface: can0\nbogus: 1\n", c, &err));
  CHECK(err.find("line 2") != std::string::npos);
  CHECK(!parse_config_text("auto_takeover: maybe\n", c, &err));
  CHECK(!parse_config_text("just a line\n", c, &err));
  CHECK(!parse_config_text("rt_priority: 200\n", c, &err));
  CHECK(!load_config_file("/nonexistent/anv.yaml", c, &err));
}

TEST(failed_parse_leaves_the_config_untouched) {
  GatewayConfig c;
  c.host_id = 2;
  std::string err;
  CHECK(!parse_config_text("host_id: 1\nbogus: 1\n", c, &err));
  CHECK_EQ(c.host_id, 2);
}

int main() { return t::run_all(); }
