// ISO-TP (ISO 15765-2) for anything over 8 bytes, on the protocol's `transfer` port.
//
// Classic CAN: every frame is 8 bytes, padded with 0xAA (as the bench node's OTA code does).
// Payloads up to 4095 bytes use the normal first frame; longer ones (up to 4 GiB) use the
// 2016 escape (length 0 then a 32-bit big-endian length). Addressing is the 29-bit ID: src and
// dst say who talks to whom, so one host can run transfers with many nodes at once.
//
// IsoTpSender and IsoTpReceiver are non-blocking state machines (feed frames and the time,
// they emit frames), for event loops. IsoTpChannel wraps both around a Link with blocking
// send()/recv(), for tools and tests.
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "anv/link.hpp"
#include "anv/schema.hpp"

namespace anv {

struct IsoTpConfig {
  uint8_t block_size = 0;  // receiving: frames per flow control we ask for (0 = no limit)
  uint8_t st_min = 0;      // receiving: gap between frames we ask for (ISO-TP STmin code)
  int timeout_ms = 1000;   // N_Bs (waiting for flow control) and N_Cr (waiting for the next frame)
  uint32_t max_rx = 65536; // receiving: longer transfers are refused with flow control OVERFLOW
  int max_wait_frames = 16;// sending: flow control WAIT frames accepted in a row
  uint8_t pad = 0xAA;
};

enum class IsoTpResult {
  Pending,     // still going
  Done,
  Timeout,     // the other side went quiet
  Overflow,    // the receiver has no room for this length
  LostFrame,   // a consecutive frame was missing or out of order
  TooManyWaits,
  BadFrame,    // malformed (e.g. a first frame announcing a length that fits a single frame)
};
const char* to_string(IsoTpResult r);

using IsoTpEmit = std::function<bool(const std::array<uint8_t, 8>&)>;

// STmin code -> microseconds (reserved codes mean the maximum, 127 ms).
uint32_t isotp_stmin_us(uint8_t code);

class IsoTpSender {
 public:
  explicit IsoTpSender(IsoTpConfig config = {}) : cfg_(config) {}
  // Starts a transfer (aborting any in progress) and sends its first frame.
  IsoTpResult start(const uint8_t* payload, size_t len, int64_t now_us, const IsoTpEmit& emit);
  // Feed every frame from the peer; only flow control frames matter.
  IsoTpResult on_frame(const uint8_t* data, uint8_t len, int64_t now_us, const IsoTpEmit& emit);
  // Call often: sends consecutive frames as STmin allows and checks the timeout.
  IsoTpResult poll(int64_t now_us, const IsoTpEmit& emit);
  IsoTpResult result() const { return result_; }
  // When poll() next has something to do (for sleeping between calls).
  int64_t next_due_us() const { return next_us_; }
  size_t sent_bytes() const { return pos_; }

 private:
  IsoTpResult finish(IsoTpResult r);
  IsoTpConfig cfg_;
  std::vector<uint8_t> payload_;
  size_t pos_ = 0;
  uint8_t sn_ = 0;
  enum class State { Idle, WaitFc, Sending } state_ = State::Idle;
  uint8_t block_size_ = 0, in_block_ = 0;
  uint32_t stmin_us_ = 0;
  int waits_ = 0;
  int64_t deadline_us_ = 0, next_us_ = 0;
  IsoTpResult result_ = IsoTpResult::Done;
};

class IsoTpReceiver {
 public:
  explicit IsoTpReceiver(IsoTpConfig config = {}) : cfg_(config) {}
  // Feed every frame from the peer. Returns Done with *payload filled when a transfer
  // completes; an error when one fails (the receiver is then ready for the next).
  IsoTpResult on_frame(const uint8_t* data, uint8_t len, int64_t now_us, const IsoTpEmit& emit,
                       std::vector<uint8_t>* payload);
  IsoTpResult poll(int64_t now_us);  // Timeout once if a transfer stalls
  bool busy() const { return active_; }
  size_t received_bytes() const { return buf_.size(); }

 private:
  bool flow_control(const IsoTpEmit& emit, uint8_t status);
  IsoTpConfig cfg_;
  bool active_ = false;
  std::vector<uint8_t> buf_;
  size_t expected_ = 0;
  uint8_t sn_ = 0, in_block_ = 0;
  int64_t deadline_us_ = 0;
};

// Blocking ISO-TP with one peer over a Link. Frames on the link that aren't this channel's
// go to `other` (if set) so a tool can keep decoding the rest of the traffic.
class IsoTpChannel {
 public:
  IsoTpChannel(Link& link, const Schema& schema, uint32_t local_id, uint32_t peer_id, IsoTpConfig config = {});

  IsoTpResult send(const std::vector<uint8_t>& payload, int timeout_ms = 10000);
  IsoTpResult recv(std::vector<uint8_t>& payload, int timeout_ms);
  std::function<void(const Frame&)> other;

  uint32_t tx_id() const { return tx_id_; }
  uint32_t rx_id() const { return rx_id_; }

 private:
  bool mine(const Frame& f) const { return f.extended && f.id == rx_id_ && !f.is_error; }
  IsoTpEmit emitter();
  Link& link_;
  uint32_t tx_id_, rx_id_;
  IsoTpSender tx_;
  IsoTpReceiver rx_;
};

int64_t monotonic_us();

}  // namespace anv
