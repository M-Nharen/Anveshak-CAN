// ID allocator: stable MAC -> ID pairs, pool order, persistence and the reserved IDs.
#include <cstdio>
#include <fstream>
#include <unistd.h>

#include "anv_gateway/id_allocator.hpp"
#include "test_util.hpp"

using namespace anv::gw;

namespace {

std::string temp_table() {
  char path[] = "/tmp/anv_id_table_XXXXXX";
  const int fd = mkstemp(path);
  if (fd >= 0) close(fd);
  std::remove(path);  // start from "no file yet", like a fresh RPi
  return path;
}

auto nothing_in_use = [](uint8_t) { return false; };

}  // namespace

TEST(new_boards_get_pool_ids_in_order_and_the_same_board_keeps_its_id) {
  IdAllocator a("");
  CHECK_EQ(a.assign(0x111, 0, nothing_in_use, nullptr), 16);
  CHECK_EQ(a.assign(0x222, 1, nothing_in_use, nullptr), 17);
  CHECK_EQ(a.assign(0x111, 0, nothing_in_use, nullptr), 16);  // asked again (lost its NVS): same ID
  CHECK_EQ(a.assign(0x333, 1, nothing_in_use, nullptr), 18);
  CHECK_EQ(a.find_by_node(17)->mac, 0x222u);
  CHECK_EQ(a.find_by_node(17)->hw_target, 1);
}

TEST(the_pool_skips_fpga_power_and_ids_already_on_the_bus) {
  const auto pool = IdAllocator::pool();
  CHECK_EQ(pool.size(), 13u);  // 16-30 minus 20 and 21
  for (uint8_t id : pool) CHECK(id != kNodeFpga && id != kNodePower);

  IdAllocator a("");
  auto busy = [](uint8_t id) { return id == 16 || id == 17; };  // boards that kept an old ID
  CHECK_EQ(a.assign(0xAAA, 0, busy, nullptr), 18);
  for (uint64_t mac = 1; mac < 13; ++mac) a.assign(mac, 0, nothing_in_use, nullptr);
  CHECK_EQ(a.assign(0xFFFF, 0, nothing_in_use, nullptr), 0);  // exhausted
}

TEST(the_table_survives_a_restart_and_forget_frees_an_id) {
  const std::string path = temp_table();
  {
    IdAllocator a(path);
    std::string err;
    CHECK(a.load(&err));
    CHECK_EQ(a.assign(0xA4CF12345678ull, 1, nothing_in_use, &err), 16);
    CHECK_EQ(a.assign(0x246F28000001ull, 0, nothing_in_use, &err), 17);
    CHECK(err.empty());
  }
  {
    IdAllocator b(path);
    std::string err;
    CHECK(b.load(&err));
    CHECK_EQ(b.table().size(), 2u);
    CHECK_EQ(b.assign(0x246F28000001ull, 0, nothing_in_use, nullptr), 17);
    CHECK_EQ(b.find_by_node(16)->hw_target, 1);
    CHECK(b.forget_node(16, &err));
    CHECK(!b.forget_node(16, &err));
    CHECK_EQ(b.assign(0x999, 0, nothing_in_use, nullptr), 16);  // reused
  }
  std::remove(path.c_str());
}

TEST(a_corrupt_table_is_reported_not_thrown) {
  const std::string path = temp_table();
  {
    std::ofstream f(path);
    f << "# comment\n\nzz-not-hex 16\n";
  }
  IdAllocator a(path);
  std::string err;
  CHECK(!a.load(&err));
  CHECK(!err.empty());
  std::remove(path.c_str());
  CHECK(IdAllocator::mac_to_string(0xA4CF12345678ull) == "a4:cf:12:34:56:78");
}

int main() { return t::run_all(); }
