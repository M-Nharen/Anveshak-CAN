// Parameter SERVICE requests (get/set/save/restore) and the Rates page's host-side settings
// (control cycle, host heartbeat): floor/ceiling enforcement and a round trip against a
// hand-rolled responder on the fake bus.
#include <cmath>

#include "rover.h"
#include "test_util.hpp"

using namespace anv::gw;
using namespace t;

namespace {

// Answers exactly one SERVICE request addressed to drive node 1 (node_id 4) from host `host`,
// echoing back `value_raw` as the current value with status OK.
class Drive1ServiceResponder {
 public:
  explicit Drive1ServiceResponder(FakeBusHub& hub) : port_(hub.connect()), thread_([this] { loop(); }) {}
  ~Drive1ServiceResponder() {
    running_ = false;
    port_->close();
    if (thread_.joinable()) thread_.join();
  }

 private:
  void loop() {
    CanFrame f;
    while (running_.load()) {
      if (port_->recv(f, 20) != RecvStatus::Ok) continue;
      Decoded d;
      if (!decode_frame(f, d)) continue;
      const ServiceMsg* req = std::get_if<ServiceMsg>(&d.msg);
      if (!req || req->is_response || req->node_id != kNodeFirstDrive) continue;
      CanFrame resp;
      const uint32_t echoed = req->value_raw != 0 ? req->value_raw : 42u;
      switch (req->host) {
        case 1: respond<rover_drive1_service_resp_orin_t>(resp, ROVER_DRIVE1_SERVICE_RESP_ORIN_FRAME_ID,
                                                            ROVER_DRIVE1_SERVICE_RESP_ORIN_LENGTH,
                                                            rover_drive1_service_resp_orin_pack,
                                                            rover_drive1_service_resp_orin_init, *req, echoed);
          break;
        case 2: respond<rover_drive1_service_resp_rpi_t>(resp, ROVER_DRIVE1_SERVICE_RESP_RPI_FRAME_ID,
                                                          ROVER_DRIVE1_SERVICE_RESP_RPI_LENGTH,
                                                          rover_drive1_service_resp_rpi_pack,
                                                          rover_drive1_service_resp_rpi_init, *req, echoed);
          break;
        default: continue;
      }
      port_->send(resp);
      return;  // one request is enough for these tests
    }
  }

  template <class S, class Pack, class Init>
  void respond(CanFrame& out, uint16_t id, uint8_t len, Pack pack, Init init, const ServiceMsg& req, uint32_t value) {
    S s;
    init(&s);
    s.opcode = req.opcode;
    s.status = 0;
    s.transaction_id = req.transaction_id;
    s.param_index = req.param_index;
    s.value_raw = value;
    out.id = id;
    out.dlc = len;
    pack(out.data.data(), &s, out.data.size());
  }

  std::unique_ptr<ICanBus> port_;
  std::atomic<bool> running_{true};
  std::thread thread_;
};

}  // namespace

TEST(set_cycle_rate_hz_enforces_the_25_to_50_hz_floor_and_ceiling) {
  FakeBusHub hub;
  Gateway gw(host_config(1), hub.connect());
  gw.start();

  CHECK(gw.set_cycle_rate_hz(24.9) == CmdResult::BadArgument);
  CHECK(gw.set_cycle_rate_hz(50.1) == CmdResult::BadArgument);
  CHECK(gw.set_cycle_rate_hz(25.0) == CmdResult::Ok);
  CHECK(std::abs(gw.state().cycle_hz - 25.0) < 0.01);
  CHECK(gw.set_cycle_rate_hz(50.0) == CmdResult::Ok);
  CHECK(std::abs(gw.state().cycle_hz - 50.0) < 0.01);

  gw.stop();
}

TEST(set_host_hb_rate_hz_enforces_the_20_hz_floor) {
  FakeBusHub hub;
  Gateway gw(host_config(1), hub.connect());
  gw.start();

  CHECK(gw.set_host_hb_rate_hz(19.9) == CmdResult::BadArgument);
  CHECK(gw.set_host_hb_rate_hz(20.0) == CmdResult::Ok);
  CHECK(std::abs(gw.state().host_hb_hz - 20.0) < 0.01);

  gw.stop();
}

TEST(service_request_times_out_with_no_node_on_the_bus) {
  FakeBusHub hub;
  Gateway gw(host_config(1), hub.connect());
  gw.start();

  const ServiceResult r = gw.service_request(kNodeFirstDrive, ServiceOpcode::Get, 7, 0, 50);
  CHECK(r.result == CmdResult::Timeout);

  gw.stop();
}

TEST(service_request_bad_node_id_is_rejected_without_touching_the_bus) {
  FakeBusHub hub;
  Gateway gw(host_config(1), hub.connect());
  gw.start();

  CHECK(gw.service_request(kNodeOrin, ServiceOpcode::Get, 0, 0, 50).result == CmdResult::BadArgument);
  CHECK(gw.service_request(kNodeFpga, ServiceOpcode::Get, 0, 0, 50).result == CmdResult::BadArgument);

  gw.stop();
}

TEST(service_request_round_trips_with_a_responding_node) {
  FakeBusHub hub;
  Gateway gw(host_config(1), hub.connect());
  Drive1ServiceResponder node(hub);
  gw.start();

  const ServiceResult r = gw.service_request(kNodeFirstDrive, ServiceOpcode::Get, 7, 0, 500);
  CHECK(r.result == CmdResult::Ok);
  CHECK_EQ(r.status, 0);
  CHECK_EQ(r.value_raw, 42u);

  gw.stop();
}

TEST(service_request_set_echoes_the_value_back) {
  FakeBusHub hub;
  Gateway gw(host_config(1), hub.connect());
  Drive1ServiceResponder node(hub);
  gw.start();

  const ServiceResult r = gw.service_request(kNodeFirstDrive, ServiceOpcode::Set, 7, 1234, 500);
  CHECK(r.result == CmdResult::Ok);
  CHECK_EQ(r.status, 0);
  CHECK_EQ(r.value_raw, 1234u);

  gw.stop();
}

int main() { return t::run_all(); }
