#include <thread>

#include "anv/test_bus.hpp"
#include "check.hpp"

using namespace anv;

namespace {
Frame ext(uint32_t id, uint8_t b0 = 0) { return Frame::make(id, &b0, 1); }
}  // namespace

TEST(delivers_to_others_not_self) {
  TestBus bus;
  auto a = bus.connect(), b = bus.connect(), c = bus.connect();
  CHECK(a->send(ext(0x1234567, 9)));
  Frame f;
  CHECK(b->recv(f, 100) == RecvStatus::Ok && f.id == 0x1234567 && f.data[0] == 9 && f.extended && f.rx_ns > 0);
  CHECK(c->recv(f, 100) == RecvStatus::Ok);
  CHECK(a->recv(f, 0) == RecvStatus::Timeout);
  CHECK(a->stats().tx_frames == 1 && b->stats().rx_frames == 1);
  CHECK(bus.frames_sent() == 1);
}

TEST(filters) {
  TestBus bus;
  auto a = bus.connect(), b = bus.connect();
  // "for me (dst 20) or broadcast" on the dst bits 25..18, as the ESP32 hardware filter does
  const uint32_t dst_mask = 0xFFu << 18;
  CHECK(b->set_filters({{20u << 18, dst_mask, true}, {0, dst_mask, true}}));
  a->send(ext(21u << 18 | 5));
  a->send(ext(20u << 18 | 6));
  a->send(ext(7));
  a->send(Frame::make(0x020, nullptr, 0, false));  // standard frame: no filter matches
  Frame f;
  CHECK(b->recv(f, 50) == RecvStatus::Ok && (f.id & 0xFF) == 6);
  CHECK(b->recv(f, 50) == RecvStatus::Ok && f.id == 7);
  CHECK(b->recv(f, 10) == RecvStatus::Timeout);
  CHECK(b->set_filters({}));
  a->send(Frame::make(0x020, nullptr, 0, false));
  CHECK(b->recv(f, 50) == RecvStatus::Ok && !f.extended && f.id == 0x020);
}

TEST(queue_overflow_is_counted) {
  TestBus bus(4);
  auto a = bus.connect(), b = bus.connect();
  for (int i = 0; i < 10; ++i) a->send(ext(static_cast<uint32_t>(i)));
  CHECK(b->stats().rx_dropped == 6);
  Frame f;
  int got = 0;
  while (b->recv(f, 0) == RecvStatus::Ok) ++got;
  CHECK(got == 4);
}

TEST(tap_can_lose_frames) {
  TestBus bus;
  auto a = bus.connect(), b = bus.connect();
  bus.set_tap([](const Frame& f) { return f.data[0] != 0xEE; });
  a->send(ext(1, 0xEE));
  a->send(ext(2, 0x01));
  Frame f;
  CHECK(b->recv(f, 50) == RecvStatus::Ok && f.id == 2);
}

TEST(close_wakes_recv) {
  TestBus bus;
  auto a = bus.connect();
  std::thread th([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    a->close();
  });
  Frame f;
  CHECK(a->recv(f, 5000) == RecvStatus::Closed);
  th.join();
  CHECK(!a->send(ext(1)));
  CHECK(a->stats().tx_dropped == 1);
}

TEST(cansend_text) {
  Frame f;
  std::string err;
  CHECK(from_cansend("04504001#02960038FF0000", f, &err));
  CHECK(f.extended && f.id == 0x04504001 && f.len == 7 && f.data[1] == 0x96);
  CHECK(to_cansend(f) == "04504001#02960038FF0000");
  CHECK(from_cansend("123#", f, &err) && !f.extended && f.len == 0 && to_cansend(f) == "123#");
  CHECK(from_cansend("1FFFFFFF#11.22", f, &err) && f.len == 2 && f.data[1] == 0x22);
  CHECK(!from_cansend("2FFFFFFF#00", f, &err));
  CHECK(!from_cansend("12#0", f, &err));
  CHECK(!from_cansend("nohash", f, &err));
  CHECK(!from_cansend("123#001122334455667788", f, &err));
}

TEST_MAIN()
