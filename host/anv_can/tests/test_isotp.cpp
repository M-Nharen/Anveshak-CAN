#include <atomic>
#include <numeric>
#include <thread>

#include "anv/isotp.hpp"
#include "anv/test_bus.hpp"
#include "check.hpp"

using namespace anv;

namespace {

std::vector<uint8_t> pattern(size_t n) {
  std::vector<uint8_t> v(n);
  for (size_t i = 0; i < n; ++i) v[i] = static_cast<uint8_t>(i * 7 + (i >> 8));
  return v;
}

// Host 1 sends to node 20 over the test bus; returns what node 20 received.
struct Pair {
  TestBus bus;
  std::unique_ptr<Link> host = bus.connect(), node = bus.connect();
  IsoTpConfig node_cfg;
  IsoTpResult rx_result = IsoTpResult::Pending, tx_result = IsoTpResult::Pending;
  std::vector<uint8_t> got;

  void run(const std::vector<uint8_t>& payload, IsoTpConfig host_cfg = {}) {
    IsoTpChannel h(*host, t::schema(), 1, 20, host_cfg);
    IsoTpChannel n(*node, t::schema(), 20, 1, node_cfg);
    std::thread rx([&] { rx_result = n.recv(got, 3000); });
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    tx_result = h.send(payload, 3000);
    rx.join();
  }
};

}  // namespace

TEST(single_frame) {
  Pair p;
  p.run({1, 2, 3});
  CHECK(p.tx_result == IsoTpResult::Done && p.rx_result == IsoTpResult::Done);
  CHECK((p.got == std::vector<uint8_t>{1, 2, 3}));
}

TEST(frames_are_padded_and_addressed) {
  const Schema& s = t::schema();
  TestBus bus;
  auto a = bus.connect(), b = bus.connect();
  IsoTpChannel ch(*a, s, 1, 20);
  CHECK(ch.send({0x42}, 100) == IsoTpResult::Done);
  Frame f;
  CHECK(b->recv(f, 50) == RecvStatus::Ok);
  const Message& tr = *s.protocol.message("transfer");
  const IdParts id = s.id_layout.parse(f.id);
  CHECK(id.port == tr.port && id.priority == tr.priority && id.src == 1 && id.dst == 20);
  CHECK(f.len == 8 && f.data[0] == 0x01 && f.data[1] == 0x42 && f.data[2] == 0xAA && f.data[7] == 0xAA);
}

TEST(long_transfer) {
  Pair p;
  const auto payload = pattern(4095);
  p.run(payload);
  CHECK(p.tx_result == IsoTpResult::Done && p.rx_result == IsoTpResult::Done);
  CHECK(p.got == payload);
}

TEST(escape_length_over_4095) {
  Pair p;
  const auto payload = pattern(10000);
  p.run(payload);
  CHECK(p.tx_result == IsoTpResult::Done && p.rx_result == IsoTpResult::Done);
  CHECK(p.got == payload);
}

TEST(block_size_and_stmin) {
  Pair p;
  p.node_cfg.block_size = 3;
  p.node_cfg.st_min = 0xF5;  // 500 us
  const auto payload = pattern(300);
  const int64_t t0 = monotonic_us();
  p.run(payload);
  CHECK(p.tx_result == IsoTpResult::Done && p.got == payload);
  // 43 consecutive frames, 500 us apart within each block of 3
  CHECK(monotonic_us() - t0 > 10000);
}

TEST(lost_frame_is_detected) {
  Pair p;
  int cf = 0;
  p.bus.set_tap([&](const Frame& f) { return !((f.data[0] >> 4) == 2 && ++cf == 5); });
  p.run(pattern(200));
  CHECK(p.rx_result == IsoTpResult::LostFrame);
}

TEST(out_of_order_is_detected) {
  // Feed the receiver directly: CF 1, then CF 3.
  IsoTpReceiver rx;
  int fcs = 0;
  IsoTpEmit emit = [&](const std::array<uint8_t, 8>& f) {
    fcs += (f[0] >> 4) == 3;
    return true;
  };
  std::vector<uint8_t> out;
  const uint8_t ff[8] = {0x10, 30, 0, 1, 2, 3, 4, 5};
  CHECK(rx.on_frame(ff, 8, 0, emit, &out) == IsoTpResult::Pending && fcs == 1 && rx.busy());
  const uint8_t cf1[8] = {0x21, 6, 7, 8, 9, 10, 11, 12};
  CHECK(rx.on_frame(cf1, 8, 1, emit, &out) == IsoTpResult::Pending);
  const uint8_t cf3[8] = {0x23, 0, 0, 0, 0, 0, 0, 0};
  CHECK(rx.on_frame(cf3, 8, 2, emit, &out) == IsoTpResult::LostFrame && !rx.busy());
  // a stray CF while idle is ignored; a new SF works
  CHECK(rx.on_frame(cf1, 8, 3, emit, &out) == IsoTpResult::Pending);
  const uint8_t sf[8] = {0x02, 0xAB, 0xCD, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA};
  CHECK(rx.on_frame(sf, 8, 4, emit, &out) == IsoTpResult::Done && (out == std::vector<uint8_t>{0xAB, 0xCD}));
}

TEST(receiver_timeout) {
  IsoTpConfig cfg;
  cfg.timeout_ms = 10;
  IsoTpReceiver rx(cfg);
  IsoTpEmit emit = [](const std::array<uint8_t, 8>&) { return true; };
  const uint8_t ff[8] = {0x10, 30, 0, 1, 2, 3, 4, 5};
  rx.on_frame(ff, 8, 0, emit, nullptr);
  CHECK(rx.poll(5000) == IsoTpResult::Pending);
  CHECK(rx.poll(10000) == IsoTpResult::Timeout && !rx.busy());
}

TEST(overflow_refused) {
  Pair p;
  p.node_cfg.max_rx = 100;
  p.run(pattern(500));
  CHECK(p.tx_result == IsoTpResult::Overflow && p.rx_result == IsoTpResult::Overflow);
}

TEST(sender_timeout_without_receiver) {
  TestBus bus;
  auto a = bus.connect();
  IsoTpConfig cfg;
  cfg.timeout_ms = 30;
  IsoTpChannel ch(*a, t::schema(), 1, 20, cfg);
  const int64_t t0 = monotonic_us();
  CHECK(ch.send(pattern(50), 1000) == IsoTpResult::Timeout);
  CHECK(monotonic_us() - t0 < 500000);
}

TEST(sender_honours_wait) {
  IsoTpSender tx;
  std::vector<std::array<uint8_t, 8>> sent;
  IsoTpEmit emit = [&](const std::array<uint8_t, 8>& f) {
    sent.push_back(f);
    return true;
  };
  const auto payload = pattern(19);  // 6 in the first frame, 7 + 6 in two consecutive ones
  CHECK(tx.start(payload.data(), payload.size(), 0, emit) == IsoTpResult::Pending && sent.size() == 1);
  const uint8_t wait[3] = {0x31, 0, 0};
  CHECK(tx.on_frame(wait, 3, 900000, emit) == IsoTpResult::Pending);
  CHECK(tx.poll(1500000, emit) == IsoTpResult::Pending);  // WAIT restarted the timer
  const uint8_t cts[3] = {0x30, 0, 0};
  CHECK(tx.on_frame(cts, 3, 1600000, emit) == IsoTpResult::Done && sent.size() == 3);
  CHECK(sent[1][0] == 0x21 && sent[2][0] == 0x22 && sent[2][7] == 0xAA);
}

TEST(other_traffic_is_passed_on) {
  TestBus bus;
  auto a = bus.connect(), b = bus.connect();
  IsoTpChannel ch(*b, t::schema(), 20, 1);
  int others = 0;
  ch.other = [&](const Frame&) { ++others; };
  const uint8_t x = 1;
  a->send(Frame::make(0x123456, &x, 1));
  std::vector<uint8_t> out;
  CHECK(ch.recv(out, 50) == IsoTpResult::Timeout);
  CHECK(others == 1);
}

TEST(stmin_codes) {
  CHECK(isotp_stmin_us(0) == 0);
  CHECK(isotp_stmin_us(0x7F) == 127000);
  CHECK(isotp_stmin_us(0xF1) == 100);
  CHECK(isotp_stmin_us(0xF9) == 900);
  CHECK(isotp_stmin_us(0x80) == 127000);
}

TEST_MAIN()
