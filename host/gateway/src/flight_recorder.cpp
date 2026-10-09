#include "anv_gateway/flight_recorder.hpp"

#include <cctype>
#include <chrono>
#include <cstdio>
#include <sys/stat.h>

namespace anv::gw {

void FlightRecorder::record(const CanFrame& f) {
  std::lock_guard<std::mutex> lk(mu_);
  frames_.push_back(f);
  const int64_t newest = frames_.back().rx_ns;
  while (!frames_.empty() && newest - frames_.front().rx_ns > window_ns_) frames_.pop_front();
}

size_t FlightRecorder::size() const {
  std::lock_guard<std::mutex> lk(mu_);
  return frames_.size();
}

void FlightRecorder::clear() {
  std::lock_guard<std::mutex> lk(mu_);
  frames_.clear();
}

std::string FlightRecorder::save(const std::string& dir, const std::string& reason) const {
  if (dir.empty()) return "";
  std::deque<CanFrame> snapshot;
  {
    std::lock_guard<std::mutex> lk(mu_);
    snapshot = frames_;
  }
  const auto now_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
          .count();
  std::string clean_reason = reason.empty() ? "manual" : reason;
  for (char& c : clean_reason)
    if (!std::isalnum(static_cast<unsigned char>(c))) c = '_';
  ::mkdir(dir.c_str(), 0755);  // best effort; failure surfaces when the file itself fails to open
  const std::string path = dir + "/flight_" + std::to_string(now_ms) + "_" + clean_reason + ".log";

  std::FILE* f = std::fopen(path.c_str(), "w");
  if (!f) return "";
  for (const CanFrame& fr : snapshot) {
    if (fr.is_error) {
      std::fprintf(f, "%lld ERROR err_class=0x%08x\n", static_cast<long long>(fr.rx_ns), fr.err_class);
      continue;
    }
    std::fprintf(f, "%lld %03X %u", static_cast<long long>(fr.rx_ns), fr.id, fr.dlc);
    for (uint8_t i = 0; i < fr.dlc; ++i) std::fprintf(f, " %02X", fr.data[i]);
    std::fprintf(f, "\n");
  }
  std::fclose(f);
  return path;
}

}  // namespace anv::gw
