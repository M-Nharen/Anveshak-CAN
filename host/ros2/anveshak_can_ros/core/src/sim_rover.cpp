#include "anv_ros2/sim_rover.hpp"

#include <algorithm>

#include "anv_gateway/encoder.hpp"
#include "anv_gateway/protocol.hpp"

namespace anv::ros2 {

using namespace anv::gw;

namespace {
constexpr int64_t kNodeWatchdogNs = 100 * kNsPerMs;  // command watchdog, same as the firmware
constexpr int64_t kHbPeriodNs = 100 * kNsPerMs;      // NODE_HB at 10 Hz
}  // namespace

SimulatedRover::SimulatedRover() : port_(hub_.connect()) {}

SimulatedRover::~SimulatedRover() { stop(); }

std::unique_ptr<ICanBus> SimulatedRover::connect() { return hub_.connect(); }

void SimulatedRover::start() {
  bool expected = false;
  if (!running_.compare_exchange_strong(expected, true)) return;
  boot_ns_ = mono_ns();
  thread_ = std::thread([this] { loop(); });
}

void SimulatedRover::stop() {
  if (!running_.exchange(false)) return;
  port_->close();
  if (thread_.joinable()) thread_.join();
}

void SimulatedRover::disarm(Drive& d) {
  d.state = static_cast<uint8_t>(NodeState::Disarmed);
  d.pending_a = d.pending_b = d.speed_a = d.speed_b = 0;
  d.master = 0;
}

void SimulatedRover::loop() {
  CanFrame f;
  while (running_.load()) {
    const RecvStatus st = port_->recv(f, 5);
    if (st == RecvStatus::Closed) break;
    const int64_t now = mono_ns();
    if (st == RecvStatus::Ok) {
      Decoded d;
      if (decode_frame(f, d)) on_frame(d, now);
    }
    tick(now);
  }
}

void SimulatedRover::on_frame(const Decoded& d, int64_t now) {
  if (std::get_if<EstopMsg>(&d.msg)) {
    for (Drive& dr : drives_) {
      dr.state = static_cast<uint8_t>(NodeState::Estop);
      dr.pending_a = dr.pending_b = dr.speed_a = dr.speed_b = 0;
    }
  } else if (std::get_if<EstopReleaseMsg>(&d.msg)) {
    for (Drive& dr : drives_)
      if (dr.state == static_cast<uint8_t>(NodeState::Estop)) disarm(dr);
  } else if (const auto* hb = std::get_if<HostHbMsg>(&d.msg)) {
    // Nodes follow the master and its term; a change of either disarms every node.
    if (hb->role == static_cast<uint8_t>(Role::Master) &&
        (hb->host_node != known_master_ || (hb->term & 0xFF) != known_term_)) {
      known_master_ = hb->host_node;
      known_term_ = hb->term & 0xFF;
      for (Drive& dr : drives_)
        if (dr.state == static_cast<uint8_t>(NodeState::Armed)) disarm(dr);
    }
  } else if (const auto* nmt = std::get_if<NmtMsg>(&d.msg)) {
    for (uint8_t i = 0; i < drives_.size(); ++i) {
      Drive& dr = drives_[i];
      if (nmt->target_node != 0 && nmt->target_node != drive_node_id(i + 1)) continue;
      switch (static_cast<NmtCommand>(nmt->command)) {
        case NmtCommand::Arm:
          if (dr.state == static_cast<uint8_t>(NodeState::Disarmed)) {
            dr.state = static_cast<uint8_t>(NodeState::Armed);
            dr.master = nmt->master_node_id;
            dr.term_low = nmt->master_term & 0x3;
            dr.last_cmd_ns = now;
            // ARM may overtake the master's first heartbeat; it names the master and term too.
            known_master_ = nmt->master_node_id;
            known_term_ = nmt->master_term;
          }
          break;
        case NmtCommand::Disarm:
          if (dr.state == static_cast<uint8_t>(NodeState::Armed)) disarm(dr);
          break;
        case NmtCommand::Reset:
          disarm(dr);
          break;
        default:
          break;
      }
    }
  } else if (const auto* cmd = std::get_if<DriveCmdMsg>(&d.msg)) {
    Drive& dr = drives_[cmd->drive - 1];
    if (dr.state != static_cast<uint8_t>(NodeState::Armed)) return;
    if (cmd->sender_host != dr.master || cmd->term_low != dr.term_low) {
      disarm(dr);  // a different master, or a new term, without a fresh ARM
      return;
    }
    dr.pending_a = cmd->target_a;
    dr.pending_b = cmd->target_b;
    dr.last_cmd_ns = now;
  } else if (std::get_if<SyncMsg>(&d.msg)) {
    // Commands are applied together at SYNC, then every node reports.
    for (uint8_t i = 0; i < drives_.size(); ++i) {
      Drive& dr = drives_[i];
      const bool armed = dr.state == static_cast<uint8_t>(NodeState::Armed);
      dr.speed_a = armed ? dr.pending_a : 0;
      dr.speed_b = armed ? dr.pending_b : 0;
      DriveFbAMsg fb;
      fb.drive = i + 1;
      fb.state = dr.state;
      fb.speed_a = dr.speed_a;
      fb.speed_b = dr.speed_b;
      port_->send(make_drive_fb_a(fb));
    }
  }
}

void SimulatedRover::tick(int64_t now) {
  for (uint8_t i = 0; i < drives_.size(); ++i) {
    Drive& dr = drives_[i];
    if (dr.state == static_cast<uint8_t>(NodeState::Armed) && now - dr.last_cmd_ns > kNodeWatchdogNs)
      disarm(dr);  // command watchdog
    if (now >= dr.next_hb_ns) {
      NodeHbMsg hb;
      hb.node_id = drive_node_id(i + 1);
      hb.state = dr.state;
      hb.node_type = 1;
      hb.protocol_version = 1;
      hb.uptime_s = static_cast<uint16_t>(std::min<int64_t>((now - boot_ns_) / kNsPerSec, 65535));
      port_->send(make_node_hb(hb));
      dr.next_hb_ns = now + kHbPeriodNs;
    }
  }
  if (now >= fpga_next_hb_ns_) {
    NodeHbMsg hb;
    hb.node_id = kNodeFpga;
    hb.state = static_cast<uint8_t>(NodeState::Disarmed);
    hb.node_type = 4;
    hb.protocol_version = 1;
    hb.uptime_s = static_cast<uint16_t>(std::min<int64_t>((now - boot_ns_) / kNsPerSec, 65535));
    port_->send(make_node_hb(hb));
    fpga_next_hb_ns_ = now + kHbPeriodNs;
  }
}

}  // namespace anv::ros2
