#include "anv/test_bus.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>

namespace anv {

namespace {

struct Port {
  std::deque<Frame> queue;
  std::vector<Filter> filters;
  LinkStats stats;
  bool closed = false;
};

}  // namespace

struct TestBusShared {
  std::mutex mu;
  std::condition_variable cv;
  std::vector<std::weak_ptr<Port>> ports;
  std::function<bool(const Frame&)> tap;
  size_t rx_queue = 0;
  uint64_t sent = 0;
  bool alive = true;
};

namespace {

int64_t now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

bool passes(const std::vector<Filter>& filters, const Frame& f) {
  if (filters.empty()) return true;
  for (const Filter& flt : filters) {
    if (flt.accepts(f)) return true;
  }
  return false;
}

class TestBusPort : public Link {
 public:
  TestBusPort(std::shared_ptr<TestBusShared> shared, std::shared_ptr<Port> port)
      : shared_(std::move(shared)), port_(std::move(port)) {}
  ~TestBusPort() override { close(); }

  bool send(const Frame& frame) override {
    std::function<bool(const Frame&)> tap;
    {
      std::lock_guard<std::mutex> lk(shared_->mu);
      if (port_->closed || !shared_->alive) {
        ++port_->stats.tx_dropped;
        return false;
      }
      ++port_->stats.tx_frames;
      tap = shared_->tap;
    }
    // The tap runs unlocked so it may itself use the bus.
    if (tap && !tap(frame)) return true;
    std::lock_guard<std::mutex> lk(shared_->mu);
    ++shared_->sent;
    Frame copy = frame;
    copy.rx_ns = now_ns();
    for (auto& weak : shared_->ports) {
      auto other = weak.lock();
      if (!other || other == port_ || other->closed || !passes(other->filters, copy)) continue;
      if (other->queue.size() >= shared_->rx_queue) {
        ++other->stats.rx_dropped;
        continue;
      }
      other->queue.push_back(copy);
    }
    shared_->cv.notify_all();
    return true;
  }

  RecvStatus recv(Frame& out, int timeout_ms) override {
    std::unique_lock<std::mutex> lk(shared_->mu);
    const auto ready = [&] { return port_->closed || !port_->queue.empty(); };
    if (!shared_->cv.wait_for(lk, std::chrono::milliseconds(timeout_ms < 0 ? 0 : timeout_ms), ready)) {
      return RecvStatus::Timeout;
    }
    if (port_->closed) return RecvStatus::Closed;
    out = port_->queue.front();
    port_->queue.pop_front();
    ++port_->stats.rx_frames;
    return RecvStatus::Ok;
  }

  bool set_filters(const std::vector<Filter>& filters) override {
    std::lock_guard<std::mutex> lk(shared_->mu);
    port_->filters = filters;
    return true;
  }

  LinkStats stats() const override {
    std::lock_guard<std::mutex> lk(shared_->mu);
    return port_->stats;
  }

  void close() override {
    std::lock_guard<std::mutex> lk(shared_->mu);
    port_->closed = true;
    shared_->cv.notify_all();
  }

 private:
  std::shared_ptr<TestBusShared> shared_;
  std::shared_ptr<Port> port_;
};

}  // namespace

TestBus::TestBus(size_t rx_queue) : shared_(std::make_shared<TestBusShared>()) {
  shared_->rx_queue = rx_queue;
}

TestBus::~TestBus() {
  std::lock_guard<std::mutex> lk(shared_->mu);
  shared_->alive = false;
}

std::unique_ptr<Link> TestBus::connect() {
  auto port = std::make_shared<Port>();
  std::lock_guard<std::mutex> lk(shared_->mu);
  auto& ports = shared_->ports;
  ports.erase(std::remove_if(ports.begin(), ports.end(), [](const auto& w) { return w.expired(); }),
              ports.end());
  ports.push_back(port);
  return std::make_unique<TestBusPort>(shared_, port);
}

void TestBus::set_tap(std::function<bool(const Frame&)> tap) {
  std::lock_guard<std::mutex> lk(shared_->mu);
  shared_->tap = std::move(tap);
}

uint64_t TestBus::frames_sent() const {
  std::lock_guard<std::mutex> lk(shared_->mu);
  return shared_->sent;
}

}  // namespace anv
