#include "anv_gateway/flight_recorder.hpp"

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "test_util.hpp"

using namespace anv::gw;
using namespace t;

namespace {

CanFrame frame(uint16_t id, int64_t rx_ns, uint8_t b0 = 0) {
  CanFrame f;
  f.id = id;
  f.dlc = 1;
  f.data[0] = b0;
  f.rx_ns = rx_ns;
  return f;
}

std::string slurp(const std::string& path) {
  std::ifstream in(path);
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

}  // namespace

TEST(the_buffer_keeps_only_the_configured_window) {
  FlightRecorder rec(10 * kNsPerSec);
  rec.record(frame(0x100, 0));
  rec.record(frame(0x101, 5 * kNsPerSec));
  rec.record(frame(0x102, 9 * kNsPerSec));
  CHECK_EQ(rec.size(), 3u);
  rec.record(frame(0x103, 11 * kNsPerSec));  // frame at t=0 is now older than the 10 s window
  CHECK_EQ(rec.size(), 3u);
}

TEST(save_writes_every_buffered_frame_oldest_first) {
  FlightRecorder rec;
  rec.record(frame(0x123, 1000, 0xAB));
  rec.record(frame(0x456, 2000, 0xCD));

  char dir[] = "/tmp/anv_flight_test_XXXXXX";
  CHECK(::mkdtemp(dir) != nullptr);
  const std::string path = rec.save(dir, "unit_test");
  CHECK(!path.empty());

  const std::string content = slurp(path);
  const size_t p1 = content.find("123");
  const size_t p2 = content.find("456");
  CHECK(p1 != std::string::npos);
  CHECK(p2 != std::string::npos);
  CHECK(p1 < p2);
  CHECK(content.find("AB") != std::string::npos);

  std::remove(path.c_str());
  ::rmdir(dir);
}

TEST(save_with_no_directory_does_nothing) {
  FlightRecorder rec;
  rec.record(frame(0x1, 0));
  CHECK(rec.save("", "x").empty());
}

TEST(clear_empties_the_buffer) {
  FlightRecorder rec;
  rec.record(frame(0x1, 0));
  rec.record(frame(0x2, 1));
  CHECK_EQ(rec.size(), 2u);
  rec.clear();
  CHECK_EQ(rec.size(), 0u);
}

int main() { return t::run_all(); }
