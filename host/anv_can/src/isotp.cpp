#include "anv/isotp.hpp"

#include <algorithm>
#include <chrono>
#include <thread>

namespace anv {

namespace {

constexpr uint8_t kSingle = 0x0, kFirst = 0x1, kConsecutive = 0x2, kFlow = 0x3;
constexpr uint8_t kFcCts = 0, kFcWait = 1, kFcOverflow = 2;
constexpr size_t kMaxNormalFirst = 4095;

// A function, so 32-bit builds (size_t is 32 bits there) don't warn that this is never true.
bool over_32_bits(uint64_t n) { return n > 0xFFFFFFFFu; }

std::array<uint8_t, 8> padded(uint8_t pad) {
  std::array<uint8_t, 8> f;
  f.fill(pad);
  return f;
}

}  // namespace

const char* to_string(IsoTpResult r) {
  switch (r) {
    case IsoTpResult::Pending: return "pending";
    case IsoTpResult::Done: return "done";
    case IsoTpResult::Timeout: return "timeout";
    case IsoTpResult::Overflow: return "overflow";
    case IsoTpResult::LostFrame: return "lost frame";
    case IsoTpResult::TooManyWaits: return "too many waits";
    case IsoTpResult::BadFrame: return "bad frame";
  }
  return "?";
}

uint32_t isotp_stmin_us(uint8_t code) {
  if (code <= 0x7F) return code * 1000u;
  if (code >= 0xF1 && code <= 0xF9) return (code - 0xF0u) * 100u;
  return 127000u;
}

int64_t monotonic_us() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// --- sender -------------------------------------------------------------------------------

IsoTpResult IsoTpSender::finish(IsoTpResult r) {
  state_ = State::Idle;
  result_ = r;
  return r;
}

IsoTpResult IsoTpSender::start(const uint8_t* payload, size_t len, int64_t now_us, const IsoTpEmit& emit) {
  payload_.assign(payload, payload + len);
  pos_ = 0;
  sn_ = 1;
  waits_ = 0;
  if (len == 0 || over_32_bits(len)) return finish(IsoTpResult::BadFrame);
  auto f = padded(cfg_.pad);
  if (len <= 7) {
    f[0] = static_cast<uint8_t>(len);
    std::copy(payload, payload + len, f.begin() + 1);
    if (!emit(f)) return finish(IsoTpResult::Timeout);
    pos_ = len;
    return finish(IsoTpResult::Done);
  }
  size_t header;
  if (len <= kMaxNormalFirst) {
    f[0] = static_cast<uint8_t>(kFirst << 4 | (len >> 8));
    f[1] = static_cast<uint8_t>(len);
    header = 2;
  } else {
    f[0] = kFirst << 4;
    f[1] = 0;
    for (int i = 0; i < 4; ++i) f[2 + i] = static_cast<uint8_t>(len >> (24 - 8 * i));
    header = 6;
  }
  pos_ = 8 - header;
  std::copy(payload, payload + pos_, f.begin() + static_cast<long>(header));
  if (!emit(f)) return finish(IsoTpResult::Timeout);
  state_ = State::WaitFc;
  deadline_us_ = now_us + int64_t{cfg_.timeout_ms} * 1000;
  next_us_ = deadline_us_;
  return result_ = IsoTpResult::Pending;
}

IsoTpResult IsoTpSender::on_frame(const uint8_t* data, uint8_t len, int64_t now_us, const IsoTpEmit& emit) {
  if (state_ != State::WaitFc || len < 3 || data[0] >> 4 != kFlow) return result_;
  switch (data[0] & 0x0F) {
    case kFcCts:
      block_size_ = data[1];
      stmin_us_ = isotp_stmin_us(data[2]);
      in_block_ = 0;
      waits_ = 0;
      state_ = State::Sending;
      next_us_ = now_us;
      deadline_us_ = now_us;  // while sending: the time of the last progress
      return poll(now_us, emit);
    case kFcWait:
      if (++waits_ > cfg_.max_wait_frames) return finish(IsoTpResult::TooManyWaits);
      deadline_us_ = now_us + int64_t{cfg_.timeout_ms} * 1000;
      next_us_ = deadline_us_;
      return result_;
    case kFcOverflow:
      return finish(IsoTpResult::Overflow);
    default:
      return finish(IsoTpResult::BadFrame);
  }
}

IsoTpResult IsoTpSender::poll(int64_t now_us, const IsoTpEmit& emit) {
  if (state_ == State::WaitFc) {
    return now_us >= deadline_us_ ? finish(IsoTpResult::Timeout) : result_;
  }
  while (state_ == State::Sending && now_us >= next_us_) {
    auto f = padded(cfg_.pad);
    f[0] = static_cast<uint8_t>(kConsecutive << 4 | sn_);
    const size_t n = std::min<size_t>(7, payload_.size() - pos_);
    std::copy(payload_.begin() + static_cast<long>(pos_), payload_.begin() + static_cast<long>(pos_ + n),
              f.begin() + 1);
    if (!emit(f)) {
      next_us_ = now_us + 1000;  // TX queue full: try again shortly, within the timeout
      return now_us >= deadline_us_ + int64_t{cfg_.timeout_ms} * 1000 ? finish(IsoTpResult::Timeout) : result_;
    }
    pos_ += n;
    sn_ = static_cast<uint8_t>((sn_ + 1) & 0x0F);
    deadline_us_ = now_us;
    if (pos_ >= payload_.size()) return finish(IsoTpResult::Done);
    if (block_size_ != 0 && ++in_block_ == block_size_) {
      state_ = State::WaitFc;
      deadline_us_ = now_us + int64_t{cfg_.timeout_ms} * 1000;
      next_us_ = deadline_us_;
      return result_;
    }
    next_us_ = now_us + stmin_us_;
  }
  return result_;
}

// --- receiver -----------------------------------------------------------------------------

bool IsoTpReceiver::flow_control(const IsoTpEmit& emit, uint8_t status) {
  auto f = padded(cfg_.pad);
  f[0] = static_cast<uint8_t>(kFlow << 4 | status);
  f[1] = cfg_.block_size;
  f[2] = cfg_.st_min;
  return emit(f);
}

IsoTpResult IsoTpReceiver::on_frame(const uint8_t* data, uint8_t len, int64_t now_us, const IsoTpEmit& emit,
                                    std::vector<uint8_t>* payload) {
  if (len == 0) return IsoTpResult::Pending;
  const uint8_t type = data[0] >> 4;
  if (type == kSingle) {
    const uint8_t n = data[0] & 0x0F;
    if (n == 0 || n > 7 || n + 1u > len) return IsoTpResult::BadFrame;
    active_ = false;  // a new message replaces one in progress (ISO 15765-2)
    if (payload) payload->assign(data + 1, data + 1 + n);
    return IsoTpResult::Done;
  }
  if (type == kFirst) {
    if (len < 8) return IsoTpResult::BadFrame;
    size_t total = static_cast<size_t>(data[0] & 0x0F) << 8 | data[1];
    size_t header = 2;
    if (total == 0) {
      total = static_cast<size_t>(data[2]) << 24 | static_cast<size_t>(data[3]) << 16 |
              static_cast<size_t>(data[4]) << 8 | data[5];
      header = 6;
      if (total <= kMaxNormalFirst) return IsoTpResult::BadFrame;
    } else if (total <= 7) {
      return IsoTpResult::BadFrame;
    }
    active_ = false;
    if (total > cfg_.max_rx) {
      flow_control(emit, kFcOverflow);
      return IsoTpResult::Overflow;
    }
    buf_.assign(data + header, data + 8);
    expected_ = total;
    sn_ = 1;
    in_block_ = 0;
    active_ = true;
    deadline_us_ = now_us + int64_t{cfg_.timeout_ms} * 1000;
    flow_control(emit, kFcCts);
    return IsoTpResult::Pending;
  }
  if (type == kConsecutive && active_) {
    if ((data[0] & 0x0F) != sn_) {
      active_ = false;
      return IsoTpResult::LostFrame;
    }
    const size_t n = std::min<size_t>({7, expected_ - buf_.size(), static_cast<size_t>(len - 1)});
    buf_.insert(buf_.end(), data + 1, data + 1 + n);
    sn_ = static_cast<uint8_t>((sn_ + 1) & 0x0F);
    deadline_us_ = now_us + int64_t{cfg_.timeout_ms} * 1000;
    if (buf_.size() >= expected_) {
      active_ = false;
      if (payload) payload->swap(buf_);
      buf_.clear();
      return IsoTpResult::Done;
    }
    if (cfg_.block_size != 0 && ++in_block_ == cfg_.block_size) {
      in_block_ = 0;
      flow_control(emit, kFcCts);
    }
  }
  return IsoTpResult::Pending;
}

IsoTpResult IsoTpReceiver::poll(int64_t now_us) {
  if (active_ && now_us >= deadline_us_) {
    active_ = false;
    return IsoTpResult::Timeout;
  }
  return IsoTpResult::Pending;
}

// --- blocking channel ---------------------------------------------------------------------

IsoTpChannel::IsoTpChannel(Link& link, const Schema& schema, uint32_t local_id, uint32_t peer_id,
                           IsoTpConfig config)
    : link_(link), tx_id_(0), rx_id_(0), tx_(config), rx_(config) {
  if (const Message* m = schema.protocol.message("transfer")) {
    tx_id_ = schema.id_layout.make(m->priority, peer_id, m->port, local_id);
    rx_id_ = schema.id_layout.make(m->priority, local_id, m->port, peer_id);
  }
}

IsoTpEmit IsoTpChannel::emitter() {
  return [this](const std::array<uint8_t, 8>& d) { return link_.send(Frame::make(tx_id_, d.data(), 8)); };
}

IsoTpResult IsoTpChannel::send(const std::vector<uint8_t>& payload, int timeout_ms) {
  if (tx_id_ == 0 && rx_id_ == 0) return IsoTpResult::BadFrame;  // schema has no transfer port
  const IsoTpEmit emit = emitter();
  const int64_t give_up = monotonic_us() + int64_t{timeout_ms} * 1000;
  IsoTpResult r = tx_.start(payload.data(), payload.size(), monotonic_us(), emit);
  while (r == IsoTpResult::Pending) {
    const int64_t now = monotonic_us();
    if (now >= give_up) return IsoTpResult::Timeout;
    const int64_t wait_us = tx_.next_due_us() - now;
    Frame f;
    if (wait_us > 1000) {
      const int wait_ms = static_cast<int>(std::min<int64_t>(wait_us / 1000, 20));
      if (link_.recv(f, wait_ms) == RecvStatus::Ok) {
        if (mine(f) && f.len > 0 && f.data[0] >> 4 == kFlow) {
          r = tx_.on_frame(f.data.data(), f.len, monotonic_us(), emit);
          continue;
        }
        if (other) other(f);
      }
    } else if (wait_us > 0) {
      std::this_thread::sleep_for(std::chrono::microseconds(wait_us));
    }
    r = tx_.poll(monotonic_us(), emit);
  }
  return r;
}

IsoTpResult IsoTpChannel::recv(std::vector<uint8_t>& payload, int timeout_ms) {
  if (tx_id_ == 0 && rx_id_ == 0) return IsoTpResult::BadFrame;
  const IsoTpEmit emit = emitter();
  const int64_t give_up = monotonic_us() + int64_t{timeout_ms} * 1000;
  for (;;) {
    const int64_t now = monotonic_us();
    if (now >= give_up) return IsoTpResult::Timeout;
    const IsoTpResult t = rx_.poll(now);
    if (t != IsoTpResult::Pending) return t;
    Frame f;
    const int wait_ms = static_cast<int>(std::min<int64_t>((give_up - now) / 1000 + 1, 20));
    const RecvStatus st = link_.recv(f, wait_ms);
    if (st == RecvStatus::Closed) return IsoTpResult::Timeout;
    if (st != RecvStatus::Ok) continue;
    if (!mine(f)) {
      if (other) other(f);
      continue;
    }
    const IsoTpResult r = rx_.on_frame(f.data.data(), f.len, monotonic_us(), emit, &payload);
    if (r != IsoTpResult::Pending) return r;
  }
}

}  // namespace anv
