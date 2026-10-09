// The Unix-socket bridge on a gateway running on the in-process fake bus.
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "anv_gateway/bridge.hpp"
#include "test_util.hpp"

using namespace anv::gw;
using namespace t;

namespace {

class Client {
 public:
  explicit Client(const std::string& path) {
    fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
    sockaddr_un a{};
    a.sun_family = AF_UNIX;
    std::snprintf(a.sun_path, sizeof a.sun_path, "%s", path.c_str());
    ok_ = ::connect(fd_, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0;
    timeval tv{3, 0};
    ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  }
  ~Client() { if (fd_ >= 0) ::close(fd_); }
  bool ok() const { return ok_; }
  // One request line out, one reply line back ("" on timeout).
  std::string call(const std::string& req) {
    const std::string line = req + "\n";
    if (::send(fd_, line.data(), line.size(), MSG_NOSIGNAL) < 0) return "";
    std::string out;
    char c;
    while (::recv(fd_, &c, 1, 0) == 1) {
      if (c == '\n') return out;
      out.push_back(c);
    }
    return "";
  }

 private:
  int fd_ = -1;
  bool ok_ = false;
};

bool has(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

std::string socket_path() { return "/tmp/anv_bridge_test_" + std::to_string(::getpid()) + ".sock"; }

}  // namespace

TEST(bridge_answers_ping_state_and_rejects_bad_requests) {
  FakeBusHub hub;
  auto port = hub.connect();
  Gateway gw(host_config(1), hub.connect());
  gw.start();
  Bridge bridge(gw, socket_path());
  std::string err;
  CHECK(bridge.start(&err));

  // A drive node's heartbeat, so the state has one node.
  NodeHbMsg hb;
  hb.node_id = 4;
  hb.state = 1;
  hb.fw_build_id = 0xBEEF;
  port->send(make_node_hb(hb));
  sleep_ms(100);

  Client c(socket_path());
  CHECK(c.ok());
  CHECK(has(c.call("{\"id\":1,\"cmd\":\"ping\"}"), "\"ok\":true"));
  const std::string st = c.call("{\"id\":2,\"cmd\":\"state\"}");
  CHECK(has(st, "\"id\":2"));
  CHECK(has(st, "\"host_id\":1"));
  CHECK(has(st, "\"role_name\":\"STANDBY\""));
  CHECK(has(st, "\"node_id\":4"));
  CHECK(has(st, "\"fw_build_id\":48879"));
  CHECK(has(st, "\"alive\":true"));

  CHECK(has(c.call("not json"), "bad json"));
  CHECK(has(c.call("{\"id\":3}"), "missing cmd"));
  CHECK(has(c.call("{\"id\":4,\"cmd\":\"fly\"}"), "unknown cmd"));
  CHECK(has(c.call("{\"id\":5,\"cmd\":\"request_lead\",\"host\":7}"), "host must be 1 or 2"));
  CHECK(has(c.call("{\"id\":6,\"cmd\":\"set_targets\",\"wheels\":[]}"), "wheels must be an array of 6"));
  bridge.stop();
  gw.stop();
}

TEST(bridge_commands_reach_the_gateway) {
  FakeBusHub hub;
  Gateway gw(host_config(1), hub.connect());
  gw.start();
  Bridge bridge(gw, socket_path());
  std::string err;
  CHECK(bridge.start(&err));
  Client c(socket_path());
  CHECK(c.ok());

  // Not the leader yet: ARM is refused by the gateway, the bridge just reports it.
  const std::string refused = c.call("{\"id\":1,\"cmd\":\"arm\"}");
  CHECK(has(refused, "\"ok\":false"));
  CHECK(has(refused, "not the master") && has(refused, "\"code\":2"));

  CHECK(has(c.call("{\"id\":2,\"cmd\":\"request_lead\",\"host\":1}"), "\"ok\":true"));
  CHECK(wait_until([&] { return gw.state().role == Role::Master; }, 2000));
  CHECK(has(c.call("{\"id\":3,\"cmd\":\"arm\"}"), "\"ok\":true"));
  CHECK(gw.state().armed);

  std::string wheels = "[";
  for (int i = 0; i < 6; ++i) wheels += std::string(i ? "," : "") + "{\"mode\":2,\"a\":1.5,\"b\":-1.5,\"accel\":8}";
  wheels += "]";
  CHECK(has(c.call("{\"id\":4,\"cmd\":\"set_targets\",\"wheels\":" + wheels + "}"), "\"ok\":true"));

  CHECK(has(c.call("{\"id\":5,\"cmd\":\"estop\"}"), "\"ok\":true"));
  CHECK(wait_until([&] { return gw.state().estop_latched; }, 1000));
  CHECK(has(c.call("{\"id\":6,\"cmd\":\"state\"}"), "\"estop_latched\":true"));
  bridge.stop();
  gw.stop();
}

TEST(bridge_handles_several_clients_and_a_closed_one) {
  FakeBusHub hub;
  Gateway gw(host_config(2), hub.connect());
  gw.start();
  Bridge bridge(gw, socket_path());
  std::string err;
  CHECK(bridge.start(&err));
  {
    Client gone(socket_path());
    CHECK(gone.ok());
  }  // closes straight away
  Client a(socket_path()), b(socket_path());
  CHECK(has(a.call("{\"id\":1,\"cmd\":\"ping\"}"), "\"ok\":true"));
  CHECK(has(b.call("{\"id\":1,\"cmd\":\"ping\"}"), "\"ok\":true"));
  CHECK(has(a.call("{\"id\":2,\"cmd\":\"state\"}"), "\"host_id\":2"));
  bridge.stop();
  gw.stop();
  Client late(socket_path());
  CHECK(!late.ok());  // the socket file is gone after stop()
}

int main() { return run_all(); }
