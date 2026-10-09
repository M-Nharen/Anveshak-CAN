#include "anv_gateway/fake_bus.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>

#include "anv_gateway/time.hpp"

namespace anv::gw {

namespace {
constexpr size_t kMaxQueue = 20000;  // a stuck reader must not grow memory without bound
}  // namespace

struct FakePort {
  std::mutex mu;
  std::condition_variable cv;
  std::deque<CanFrame> queue;
  bool closed = false;
};

struct FakeBusShared {
  std::mutex mu;
  std::vector<std::shared_ptr<FakePort>> ports;
  std::atomic<uint64_t> sent{0};
};

namespace {

class FakeBusPort : public ICanBus {
 public:
  FakeBusPort(std::shared_ptr<FakeBusShared> hub, std::shared_ptr<FakePort> self)
      : hub_(std::move(hub)), self_(std::move(self)) {}
  ~FakeBusPort() override { close(); }

  bool send(const CanFrame& frame) override;
  RecvStatus recv(CanFrame& out, int timeout_ms) override;
  void close() override;

 private:
  std::shared_ptr<FakeBusShared> hub_;
  std::shared_ptr<FakePort> self_;
};

bool FakeBusPort::send(const CanFrame& frame) {
  {
    std::lock_guard<std::mutex> lk(self_->mu);
    if (self_->closed) return false;
  }
  CanFrame stamped = frame;
  stamped.rx_ns = mono_ns();
  std::vector<std::shared_ptr<FakePort>> targets;
  {
    std::lock_guard<std::mutex> lk(hub_->mu);
    targets = hub_->ports;
  }
  for (auto& p : targets) {
    if (p == self_) continue;
    {
      std::lock_guard<std::mutex> lk(p->mu);
      if (p->closed) continue;
      if (p->queue.size() >= kMaxQueue) p->queue.pop_front();
      p->queue.push_back(stamped);
    }
    p->cv.notify_one();
  }
  hub_->sent.fetch_add(1, std::memory_order_relaxed);
  return true;
}

RecvStatus FakeBusPort::recv(CanFrame& out, int timeout_ms) {
  std::unique_lock<std::mutex> lk(self_->mu);
  if (!self_->cv.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                          [&] { return self_->closed || !self_->queue.empty(); })) {
    return RecvStatus::Timeout;
  }
  if (self_->closed) return RecvStatus::Closed;
  out = self_->queue.front();
  self_->queue.pop_front();
  return RecvStatus::Ok;
}

void FakeBusPort::close() {
  {
    std::lock_guard<std::mutex> lk(self_->mu);
    if (self_->closed) return;
    self_->closed = true;
  }
  self_->cv.notify_all();
  std::lock_guard<std::mutex> lk(hub_->mu);
  hub_->ports.erase(std::remove(hub_->ports.begin(), hub_->ports.end(), self_), hub_->ports.end());
}

}  // namespace

FakeBusHub::FakeBusHub() : shared_(std::make_shared<FakeBusShared>()) {}
FakeBusHub::~FakeBusHub() = default;

std::unique_ptr<ICanBus> FakeBusHub::connect() {
  auto port = std::make_shared<FakePort>();
  {
    std::lock_guard<std::mutex> lk(shared_->mu);
    shared_->ports.push_back(port);
  }
  return std::make_unique<FakeBusPort>(shared_, port);
}

uint64_t FakeBusHub::frames_sent() const { return shared_->sent.load(std::memory_order_relaxed); }

}  // namespace anv::gw
