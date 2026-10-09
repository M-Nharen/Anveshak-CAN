#include "anv_gateway/bus_errors.hpp"

#include "anv_gateway/time.hpp"
#include "errors.h"

namespace anv::gw {

namespace {
// linux/can/error.h bit values, copied so this file needs no Linux headers (can_bus.hpp already
// does the same for kErrClassBusOff/kErrClassRestarted).
constexpr uint32_t kErrClassAck = 0x00000020u;       // CAN_ERR_ACK
constexpr uint32_t kErrClassProtocol = 0x00000008u;  // CAN_ERR_PROT, type in data[2]
constexpr uint8_t kErrProtBit = 0x01u | 0x08u | 0x10u;  // CAN_ERR_PROT_BIT / _BIT0 / _BIT1
constexpr uint8_t kErrProtForm = 0x02u;                 // CAN_ERR_PROT_FORM
constexpr uint8_t kErrProtStuff = 0x04u;                // CAN_ERR_PROT_STUFF

constexpr int64_t kBurstWindowNs = kNsPerSec;
constexpr uint32_t kBurstThreshold = 10;  // docs: "more than 10 bus error frames in a second"
}  // namespace

BusErrorType classify_bus_error(const CanFrame& f) {
  if (f.err_class & kErrClassAck) return BusErrorType::Ack;
  if (f.err_class & kErrClassProtocol) {
    const uint8_t prot = f.data[2];
    if (prot & kErrProtStuff) return BusErrorType::Stuff;
    if (prot & kErrProtForm) return BusErrorType::Form;
    if (prot & kErrProtBit) return BusErrorType::Bit;
  }
  return BusErrorType::Other;
}

const char* bus_error_type_name(BusErrorType t) {
  switch (t) {
    case BusErrorType::Ack: return "ACK";
    case BusErrorType::Stuff: return "STUFF";
    case BusErrorType::Form: return "FORM";
    case BusErrorType::Bit: return "BIT";
    case BusErrorType::Other: return "OTHER";
  }
  return "?";
}

const char* bus_error_likely_cause(BusErrorType t) {
  switch (t) {
    case BusErrorType::Ack:
      return "The sender is alone on the bus, every other node is at a different bitrate, or a "
             "transceiver is in standby";
    case BusErrorType::Stuff:
      return "Bitrate or sample-point mismatch, missing termination, reflections from long branches";
    case BusErrorType::Form:
      return "Bitrate or sample-point mismatch, missing termination, reflections from long branches";
    case BusErrorType::Bit:
      return "Two senders on the same ID, a faulty transceiver, or a short on the bus";
    case BusErrorType::Other:
      return "Noise or reflections; check termination (should read 60 ohm) and grounding";
  }
  return "";
}

void BusErrorStats::on_error_frame(const CanFrame& f, int64_t now_ns, std::vector<GatewayEvent>& events) {
  const BusErrorType type = classify_bus_error(f);
  ++totals_[static_cast<size_t>(type)];

  if (window_start_ns_ == 0 || now_ns - window_start_ns_ > kBurstWindowNs) {
    window_start_ns_ = now_ns;
    window_count_ = 0;
    window_flagged_ = false;
  }
  ++window_count_;
  if (window_count_ > kBurstThreshold && !window_flagged_) {
    window_flagged_ = true;
    GatewayEvent e;
    e.type = EventType::NodeEvent;  // reuses the generic "numbered event" shape; node_id 0 = the bus itself
    e.time_ns = now_ns;
    e.code = ROVER_ERR_BUS_ERROR_BURST;
    e.severity = static_cast<uint8_t>(ROVER_SEVERITY_WARNING);
    e.detail = static_cast<uint16_t>(type);
    events.push_back(e);
  }
}

}  // namespace anv::gw
