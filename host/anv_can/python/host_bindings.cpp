// Python bindings for anv_host. Blocking calls release the GIL; callbacks take it back on the
// host's thread. A Python callback that raises is reported as "unraisable" and the host goes on.
#include <pybind11/functional.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <stdexcept>

#include "anv/host/host.hpp"
#include "convert.hpp"

namespace py = pybind11;
using namespace anv;
using namespace anv::host;
using namespace anv_py;

namespace {

struct HostError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

// Python owns Host through this: stop the thread without the GIL (it may be waiting for the
// GIL to run a callback), then delete with the GIL held (the callbacks hold Python objects).
struct HostDeleter {
  void operator()(Host* h) const {
    {
      py::gil_scoped_release nogil;
      h->stop();
    }
    delete h;
  }
};
using HostPtr = std::unique_ptr<Host, HostDeleter>;

double check(const Result& r) {
  if (!r.ok()) throw HostError(std::string(to_string(r.status)) + (r.error.empty() ? "" : ": " + r.error));
  return r.value;
}

void check(Status s) {
  if (s != Status::Ok) throw HostError(to_string(s));
}

template <class T>
struct no_deduce {
  using type = T;
};

// Wrap a Python callable so the host thread can call it; to_py makes its arguments.
template <class... Args>
std::function<void(Args...)> wrap(const py::object& fn,
                                  typename no_deduce<std::function<py::tuple(Args...)>>::type to_py) {
  if (fn.is_none()) return nullptr;
  auto f = std::make_shared<py::object>(fn);
  // The shared_ptr is destroyed with the GIL held: by set_callbacks' caller or by HostDeleter.
  return [f, to_py](Args... args) {
    py::gil_scoped_acquire gil;
    try {
      (*f)(*to_py(args...));
    } catch (py::error_already_set& e) {
      e.discard_as_unraisable("anv_can.Host callback");
    }
  };
}

HostConfig make_config(uint8_t host_id, uint8_t tie_break, bool observer, bool auto_takeover,
                       const std::string& allocator, uint8_t id_first, uint8_t id_last, const std::string& id_table,
                       double heartbeat_hz, double sync_hz, double command_hz, int leader_timeout_ms,
                       int node_timeout_ms, int service_timeout_ms, int service_retries) {
  HostConfig c;
  c.host_id = host_id;
  c.tie_break = tie_break;
  c.observer = observer;
  c.auto_takeover = auto_takeover;
  if (allocator == "off") c.allocator = HostConfig::Allocator::Off;
  else if (allocator == "auto") c.allocator = HostConfig::Allocator::Auto;
  else if (allocator == "always") c.allocator = HostConfig::Allocator::Always;
  else throw py::value_error("allocator must be 'off', 'auto' or 'always'");
  c.id_first = id_first;
  c.id_last = id_last;
  c.id_table_path = id_table;
  c.heartbeat_hz = heartbeat_hz;
  c.sync_hz = sync_hz;
  c.command_hz = command_hz;
  c.leader_timeout_ms = leader_timeout_ms;
  c.node_timeout_ms = node_timeout_ms;
  c.service_timeout_ms = service_timeout_ms;
  c.service_retries = service_retries;
  return c;
}

Values values_arg(const py::object& o) { return o.is_none() ? Values{} : to_values(o.cast<py::dict>()); }

}  // namespace

void bind_host(py::module_& m) {
  py::register_exception<HostError>(m, "HostError", PyExc_RuntimeError);

  py::class_<NodeInfo>(m, "NodeInfo")
      .def_readonly("id", &NodeInfo::id)
      .def_readonly("online", &NodeInfo::online)
      .def_readonly("type_id", &NodeInfo::type_id)
      .def_readonly("schema_hash", &NodeInfo::schema_hash)
      .def_property_readonly("type", [](const NodeInfo& n) { return n.type; }, py::return_value_policy::reference)
      .def_property_readonly("compat", [](const NodeInfo& n) { return to_string(n.compat); })
      .def_property_readonly("usable", [](const NodeInfo& n) { return n.usable_type() != nullptr; })
      .def_property_readonly("state", [](const NodeInfo& n) { return n.last.state; })
      .def_property_readonly("can_state", [](const NodeInfo& n) { return n.last.can_state; })
      .def_property_readonly("debug_level", [](const NodeInfo& n) { return n.last.debug_level; })
      .def_property_readonly("unsaved_params", [](const NodeInfo& n) { return n.last.unsaved_params; })
      .def_property_readonly("leader_seen", [](const NodeInfo& n) { return n.last.leader_seen; })
      .def_property_readonly("fault_active", [](const NodeInfo& n) { return n.last.fault_active; })
      .def_property_readonly("uptime_s", [](const NodeInfo& n) { return n.last.uptime_s; })
      .def_readonly("reboots", &NodeInfo::reboots)
      .def_readonly("heartbeat_hz", &NodeInfo::heartbeat_hz)
      .def_readonly("mac", &NodeInfo::mac)
      .def("__repr__", [](const NodeInfo& n) {
        return "<NodeInfo " + std::to_string(n.id) + " " + (n.type ? n.type->name : "type " + std::to_string(n.type_id)) +
               (n.online ? " online" : " offline") + ">";
      });

  py::class_<PeerHost>(m, "PeerHost")
      .def_readonly("id", &PeerHost::id)
      .def_property_readonly("role", [](const PeerHost& p) { return to_string(p.role); })
      .def_readonly("tie_break", &PeerHost::tie_break)
      .def_readonly("term", &PeerHost::term)
      .def_readonly("leader", &PeerHost::leader)
      .def_readonly("online", &PeerHost::online)
      .def("__repr__", [](const PeerHost& p) { return "<PeerHost " + std::to_string(p.id) + " " + to_string(p.role) + ">"; });

  py::class_<Host, HostPtr>(m, "Host")
      .def(py::init([](Link& link, std::shared_ptr<Schema> schema, uint8_t host_id, uint8_t tie_break, bool observer,
                       bool auto_takeover, const std::string& allocator, uint8_t id_first, uint8_t id_last,
                       const std::string& id_table, double heartbeat_hz, double sync_hz, double command_hz,
                       int leader_timeout_ms, int node_timeout_ms, int service_timeout_ms, int service_retries) {
             return HostPtr(new Host(link, schema,
                                     make_config(host_id, tie_break, observer, auto_takeover, allocator, id_first, id_last,
                                                 id_table, heartbeat_hz, sync_hz, command_hz, leader_timeout_ms,
                                                 node_timeout_ms, service_timeout_ms, service_retries)));
           }),
           py::arg("link"), py::arg("schema"), py::kw_only(), py::arg("host_id"), py::arg("tie_break") = 0,
           py::arg("observer") = false, py::arg("auto_takeover") = false, py::arg("allocator") = "auto",
           py::arg("id_first") = 0, py::arg("id_last") = 0, py::arg("id_table") = "", py::arg("heartbeat_hz") = 20.0,
           py::arg("sync_hz") = 50.0, py::arg("command_hz") = 50.0, py::arg("leader_timeout_ms") = 250,
           py::arg("node_timeout_ms") = 500, py::arg("service_timeout_ms") = 100, py::arg("service_retries") = 2,
           py::keep_alive<1, 2>(), py::keep_alive<1, 3>())
      .def("set_callbacks", [](Host& h, py::kwargs kw) {
        HostCallbacks cb;
        auto get = [&](const char* name) -> py::object {
          if (!kw.contains(name)) return py::none();
          py::object o = kw[name];
          PyDict_DelItemString(kw.ptr(), name);
          return o;
        };
        cb.node_appeared = wrap<const NodeInfo&, bool>(get("node_appeared"), [](const NodeInfo& n, bool r) {
          return py::make_tuple(n, r);
        });
        cb.node_lost = wrap<const NodeInfo&>(get("node_lost"), [](const NodeInfo& n) { return py::make_tuple(n); });
        cb.node_rebooted = wrap<const NodeInfo&>(get("node_rebooted"), [](const NodeInfo& n) { return py::make_tuple(n); });
        cb.node_state = wrap<const NodeInfo&, uint8_t>(get("node_state"), [](const NodeInfo& n, uint8_t old) {
          return py::make_tuple(n, old);
        });
        cb.telemetry = wrap<uint8_t, const Message&, const Values&, int64_t>(
            get("telemetry"), [](uint8_t node, const Message& msg, const Values& v, int64_t ts) {
              return py::make_tuple(node, msg.name, from_values(msg, v), ts);
            });
        cb.event = wrap<const NodeEvent&>(get("event"), [](const NodeEvent& e) {
          py::dict d;
          d["node"] = e.node;
          d["code"] = e.code;
          d["name"] = e.name;
          d["severity"] = e.severity;
          d["cleared"] = e.cleared;
          d["detail"] = e.detail;
          d["uptime_s"] = e.uptime_s;
          return py::make_tuple(d);
        });
        cb.setting_changed = wrap<uint8_t, const Param&, double>(
            get("setting_changed"), [](uint8_t node, const Param& p, double v) { return py::make_tuple(node, p.name, v); });
        cb.role_changed = wrap<Role, uint16_t>(get("role_changed"), [](Role r, uint16_t term) {
          return py::make_tuple(to_string(r), term);
        });
        cb.leader_changed = wrap<uint8_t, uint16_t>(get("leader_changed"), [](uint8_t l, uint16_t term) {
          return py::make_tuple(l, term);
        });
        cb.leader_lost = wrap<uint8_t>(get("leader_lost"), [](uint8_t l) { return py::make_tuple(l); });
        cb.two_leaders = wrap<uint8_t, uint8_t>(get("two_leaders"), [](uint8_t a, uint8_t b) { return py::make_tuple(a, b); });
        cb.id_assigned = wrap<uint8_t, uint64_t, uint16_t>(get("id_assigned"), [](uint8_t n, uint64_t mac, uint16_t t) {
          return py::make_tuple(n, mac, t);
        });
        cb.estop = wrap<uint8_t, uint8_t>(get("estop"), [](uint8_t src, uint8_t reason) { return py::make_tuple(src, reason); });
        if (py::len(kw) != 0) throw py::type_error("unknown callback: " + py::str(kw.begin()->first).cast<std::string>());
        if (!h.set_callbacks(std::move(cb))) throw HostError("set callbacks before start()");
      })
      .def("start", [](Host& h) {
        std::string err;
        if (!h.start(&err)) throw HostError(err);
      })
      .def("stop", &Host::stop, py::call_guard<py::gil_scoped_release>())
      .def_property_readonly("running", &Host::running)
      // state
      .def("nodes", &Host::nodes, py::call_guard<py::gil_scoped_release>())
      .def("node", &Host::node, py::call_guard<py::gil_scoped_release>())
      .def("hosts", &Host::hosts, py::call_guard<py::gil_scoped_release>())
      .def("id_table", [](const Host& h) {
        py::list l;
        for (const Assignment& a : h.id_table()) l.append(py::make_tuple(a.mac, a.node_id, a.type_id));
        return l;
      })
      .def("wait_for_node", &Host::wait_for_node, py::arg("node"), py::arg("timeout_ms"),
           py::call_guard<py::gil_scoped_release>())
      .def("wait_for_lead", &Host::wait_for_lead, py::arg("timeout_ms"), py::call_guard<py::gil_scoped_release>())
      // leadership
      .def_property_readonly("role", [](const Host& h) { return to_string(h.role()); })
      .def_property_readonly("term", &Host::term)
      .def_property_readonly("leader", &Host::leader)
      .def_property_readonly("leading", &Host::leading)
      .def_property_readonly("host_id", [](const Host& h) { return h.config().host_id; })
      .def("request_lead", [](Host& h, uint8_t who) {
        Status s;
        {
          py::gil_scoped_release nogil;
          s = h.request_lead(who);
        }
        check(s);
      }, py::arg("host_id") = 0)
      .def("resign", &Host::resign, py::call_guard<py::gil_scoped_release>())
      .def("set_operator_link", &Host::set_operator_link)
      .def("set_autonomy_ready", &Host::set_autonomy_ready)
      .def("pin_id", [](Host& h, uint64_t mac, uint8_t node, uint16_t type_id) {
        std::string err;
        if (!h.pin_id(mac, node, type_id, &err)) throw HostError(err);
      }, py::arg("mac"), py::arg("node"), py::arg("type_id"))
      // settings, rates, actions
      .def("get", [](Host& h, uint8_t node, const std::string& p) {
        Result r;
        {
          py::gil_scoped_release nogil;
          r = h.get(node, p);
        }
        return check(r);
      }, py::arg("node"), py::arg("param"))
      .def("set", [](Host& h, uint8_t node, const std::string& p, double v) {
        Result r;
        {
          py::gil_scoped_release nogil;
          r = h.set(node, p, v);
        }
        return check(r);
      }, py::arg("node"), py::arg("param"), py::arg("value"))
      .def("save", [](Host& h, uint8_t node) {
        Result r;
        {
          py::gil_scoped_release nogil;
          r = h.save(node);
        }
        check(r);
      }, py::arg("node"))
      .def("restore_defaults", [](Host& h, uint8_t node) {
        Result r;
        {
          py::gil_scoped_release nogil;
          r = h.restore_defaults(node);
        }
        check(r);
      }, py::arg("node"))
      .def("info", [](Host& h, uint8_t node, const std::string& page) {
        Result r;
        {
          py::gil_scoped_release nogil;
          r = h.info(node, page);
        }
        return static_cast<uint32_t>(check(r));
      }, py::arg("node"), py::arg("page"))
      .def("get_rate", [](Host& h, uint8_t node, const std::string& stream) {
        Result r;
        {
          py::gil_scoped_release nogil;
          r = h.get_rate(node, stream);
        }
        return check(r);
      }, py::arg("node"), py::arg("stream"))
      .def("set_rate", [](Host& h, uint8_t node, const std::string& stream, double hz) {
        Result r;
        {
          py::gil_scoped_release nogil;
          r = h.set_rate(node, stream, hz);
        }
        return check(r);
      }, py::arg("node"), py::arg("stream"), py::arg("hz"))
      .def("action", [](Host& h, uint8_t node, const std::string& name, std::optional<double> arg) {
        Result r;
        {
          py::gil_scoped_release nogil;
          r = h.action(node, name, arg);
        }
        check(r);
      }, py::arg("node"), py::arg("name"), py::arg("arg") = py::none())
      .def("find_param", &Host::find_param, py::arg("node"), py::arg("param"), py::return_value_policy::reference)
      // commands, NMT, E-stop
      .def("send_command", [](Host& h, uint8_t node, const std::string& msg, const py::object& values, bool raw) {
        const Values v = values_arg(values);
        Result r;
        {
          py::gil_scoped_release nogil;
          r = h.send_command(node, msg, v, raw);
        }
        check(r);
      }, py::arg("node"), py::arg("message"), py::arg("values") = py::none(), py::kw_only(), py::arg("raw") = false)
      .def("set_command", [](Host& h, uint8_t node, const std::string& msg, const py::object& values, double hz, bool raw) {
        const Values v = values_arg(values);
        Result r;
        {
          py::gil_scoped_release nogil;
          r = h.set_command(node, msg, v, hz, raw);
        }
        check(r);
      }, py::arg("node"), py::arg("message"), py::arg("values") = py::none(), py::arg("hz") = 0.0, py::kw_only(),
         py::arg("raw") = false)
      .def("stop_command", &Host::stop_command, py::arg("node"), py::arg("message"),
           py::call_guard<py::gil_scoped_release>())
      .def("stop_all_commands", &Host::stop_all_commands, py::call_guard<py::gil_scoped_release>())
      .def("arm", [](Host& h, uint8_t node) { check(h.arm(node)); }, py::arg("node") = 0,
           py::call_guard<py::gil_scoped_release>())
      .def("disarm", [](Host& h, uint8_t node) { check(h.disarm(node)); }, py::arg("node") = 0,
           py::call_guard<py::gil_scoped_release>())
      .def("nmt", [](Host& h, uint8_t node, const std::string& cmd) { check(h.nmt(node, cmd)); }, py::arg("node"),
           py::arg("command"), py::call_guard<py::gil_scoped_release>())
      .def("estop", [](Host& h, uint8_t reason) { check(h.estop(reason)); }, py::arg("reason") = 0,
           py::call_guard<py::gil_scoped_release>())
      .def("estop_release", [](Host& h) { check(h.estop_release()); }, py::call_guard<py::gil_scoped_release>())
      .def_property_readonly("estop_latched", &Host::estop_latched);
}
