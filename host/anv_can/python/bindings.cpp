// Python bindings for anv_link + anv_proto, so tools and the dashboard use the same code as
// the C++ host. Kept thin: the Python package (python/anv_can) adds the friendly bits.
#include <pybind11/functional.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <stdexcept>

#include "anv/codec.hpp"
#include "convert.hpp"
#include "anv/isotp.hpp"
#include "anv/test_bus.hpp"
#ifdef __linux__
#include "anv/socketcan_link.hpp"
#endif

namespace py = pybind11;
using namespace anv;
using namespace anv_py;

void bind_host(py::module_& m);  // host_bindings.cpp

namespace {

// Turned into Python exceptions by register_exception below.
struct CodecError : std::runtime_error {
  using std::runtime_error::runtime_error;
};
struct IsoTpError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

py::dict enum_dict(const EnumValues& ev) {
  py::dict d;
  for (const auto& kv : ev) d[py::str(kv.first)] = kv.second;
  return d;
}

py::bytes frame_bytes(const Frame& f) {
  return py::bytes(reinterpret_cast<const char*>(f.data.data()), f.len);
}

Frame make_frame(uint32_t id, const py::bytes& data, bool extended) {
  const std::string s = data;
  if (s.size() > 8) throw py::value_error("CAN frames carry at most 8 bytes");
  return Frame::make(id, reinterpret_cast<const uint8_t*>(s.data()), static_cast<uint8_t>(s.size()), extended);
}

py::dict stats_dict(const LinkStats& s) {
  py::dict d;
  d["tx_frames"] = s.tx_frames;
  d["rx_frames"] = s.rx_frames;
  d["tx_dropped"] = s.tx_dropped;
  d["rx_dropped"] = s.rx_dropped;
  d["error_frames"] = s.error_frames;
  d["bus_off"] = s.bus_off;
  d["restarts"] = s.restarts;
  return d;
}

std::vector<Filter> to_filters(const py::object& o) {
  std::vector<Filter> out;
  if (o.is_none()) return out;
  for (const auto& item : o) {
    const py::tuple t = py::reinterpret_borrow<py::tuple>(item);
    Filter f;
    f.id = t[0].cast<uint32_t>();
    f.mask = t[1].cast<uint32_t>();
    f.extended = t.size() > 2 ? t[2].cast<bool>() : true;
    out.push_back(f);
  }
  return out;
}

}  // namespace

PYBIND11_MODULE(_anv_can, m) {
  m.doc() = "anv_link + anv_proto (C++), for the Anveshak CAN protocol";
  py::register_exception<CodecError>(m, "CodecError", PyExc_ValueError);
  py::register_exception<IsoTpError>(m, "IsoTpError", PyExc_RuntimeError);

  // --- schema --------------------------------------------------------------------------
  py::class_<Field>(m, "Field")
      .def_readonly("name", &Field::name)
      .def_property_readonly("type", [](const Field& f) { return f.type.name; })
      .def_property_readonly("bits", [](const Field& f) { return f.type.bits; })
      .def_readonly("start_bit", &Field::start_bit)
      .def_readonly("count", &Field::count)
      .def_readonly("scale", &Field::scale)
      .def_readonly("offset", &Field::offset)
      .def_readonly("unit", &Field::unit)
      .def_readonly("min", &Field::min)
      .def_readonly("max", &Field::max)
      .def_readonly("const", &Field::const_value)
      .def_property_readonly("enum", [](const Field& f) -> py::object {
        return f.enum_values.empty() ? py::object(py::none()) : py::object(enum_dict(f.enum_values));
      })
      .def("__repr__", [](const Field& f) { return "<Field " + f.name + " " + f.type.name + ">"; });

  py::class_<Message>(m, "Message")
      .def_readonly("name", &Message::name)
      .def_readonly("port", &Message::port)
      .def_readonly("priority", &Message::priority)
      .def_readonly("sender", &Message::sender)
      .def_readonly("kind", &Message::kind)
      .def_readonly("length", &Message::length)
      .def_readonly("watchdog_ms", &Message::watchdog_ms)
      .def_readonly("apply", &Message::apply)
      .def_readonly("description", &Message::description)
      .def_property_readonly("rate", [](const Message& msg) -> py::object {
        if (!msg.rate) return py::none();
        py::dict d;
        d["default"] = msg.rate->def;
        d["min"] = msg.rate->min;
        d["max"] = msg.rate->max;
        return d;
      })
      .def_readonly("fields", &Message::fields, py::return_value_policy::reference_internal)
      .def("field", &Message::field, py::return_value_policy::reference_internal)
      .def("__repr__", [](const Message& msg) { return "<Message " + msg.name + " port " + std::to_string(msg.port) + ">"; });

  py::class_<Param>(m, "Param")
      .def_readonly("index", &Param::index)
      .def_readonly("name", &Param::name)
      .def_property_readonly("type", [](const Param& p) { return p.type.name; })
      .def_readonly("default", &Param::def)
      .def_readonly("min", &Param::min)
      .def_readonly("max", &Param::max)
      .def_readonly("unit", &Param::unit)
      .def_readonly("access", &Param::access)
      .def_readonly("group", &Param::group)
      .def_readonly("persist", &Param::persist)
      .def_readonly("disarmed_only", &Param::disarmed_only)
      .def_readonly("reboot_required", &Param::reboot_required);

  py::class_<Action>(m, "Action")
      .def_readonly("index", &Action::index)
      .def_readonly("name", &Action::name)
      .def_property_readonly("arg", [](const Action& a) -> py::object {
        return a.arg ? py::object(py::str(a.arg->name)) : py::object(py::none());
      })
      .def_readonly("disarmed_only", &Action::disarmed_only);

  py::class_<Event>(m, "Event")
      .def_readonly("code", &Event::code)
      .def_readonly("name", &Event::name)
      .def_readonly("severity", &Event::severity)
      .def_readonly("description", &Event::description);

  py::class_<NodeType>(m, "NodeType")
      .def_readonly("name", &NodeType::name)
      .def_readonly("type_id", &NodeType::type_id)
      .def_readonly("version", &NodeType::version)
      .def_readonly("wire_hash", &NodeType::wire_hash)
      .def_readonly("messages", &NodeType::messages, py::return_value_policy::reference_internal)
      .def_readonly("params", &NodeType::params, py::return_value_policy::reference_internal)
      .def_readonly("actions", &NodeType::actions, py::return_value_policy::reference_internal)
      .def_readonly("events", &NodeType::events, py::return_value_policy::reference_internal)
      .def_property_readonly("enums", [](const NodeType& t) {
        py::dict d;
        for (const auto& kv : t.enums) d[py::str(kv.first)] = enum_dict(kv.second);
        return d;
      })
      .def("message", [](const NodeType& t, const py::object& key) -> const Message* {
        const Message* msg = py::isinstance<py::int_>(key) ? t.message(key.cast<uint16_t>())
                                                            : t.message(key.cast<std::string>());
        if (!msg) throw py::key_error(py::str(key));
        return msg;
      }, py::return_value_policy::reference_internal)
      .def("param", [](const NodeType& t, const py::object& key) -> const Param* {
        const Param* p = py::isinstance<py::int_>(key) ? t.param(key.cast<uint16_t>())
                                                        : t.param(key.cast<std::string>());
        if (!p) throw py::key_error(py::str(key));
        return p;
      }, py::return_value_policy::reference_internal)
      .def("event", &NodeType::event, py::return_value_policy::reference_internal)
      .def_property_readonly("schema_hash16", &NodeType::schema_hash16)
      .def("__repr__", [](const NodeType& t) { return "<NodeType " + t.name + " id " + std::to_string(t.type_id) + ">"; });

  py::class_<Schema, std::shared_ptr<Schema>>(m, "Schema")
      .def_static("load", [](const std::string& path) {
        std::string err;
        auto s = Schema::load_file(path, &err);
        if (!s) throw py::value_error(err);
        return s;
      }, py::arg("path"))
      .def_static("loads", [](const std::string& text) {
        std::string err;
        auto s = Schema::load_json(text, &err);
        if (!s) throw py::value_error(err);
        return s;
      })
      .def_static("default_path", &Schema::default_path)
      .def_readonly("protocol", &Schema::protocol, py::return_value_policy::reference_internal)
      .def_readonly("types", &Schema::types, py::return_value_policy::reference_internal)
      .def_readonly("protocol_last_port", &Schema::protocol_last_port)
      .def_property_readonly("max_port", &Schema::max_port)
      .def_property_readonly("node_ids", [](const Schema& s) {
        py::dict d;
        d["broadcast"] = s.node_ids.broadcast;
        d["unassigned"] = s.node_ids.unassigned;
        d["first"] = s.node_ids.first;
        d["last"] = s.node_ids.last;
        return d;
      })
      .def_property_readonly("id_layout", [](const Schema& s) {
        py::list l;
        for (const auto& p : s.id_layout.parts()) l.append(py::make_tuple(p.name, p.bits, p.shift));
        return l;
      })
      .def("type", [](const Schema& s, const std::string& name) {
        const NodeType* t = s.type(name);
        if (!t) throw py::key_error(name);
        return t;
      }, py::return_value_policy::reference_internal)
      .def("type_by_id", &Schema::type_by_id, py::return_value_policy::reference_internal)
      .def("message_for", &Schema::message_for, py::arg("port"), py::arg("node_type") = nullptr,
           py::return_value_policy::reference_internal)
      .def("make_id", [](const Schema& s, uint32_t priority, uint32_t dst, uint32_t port, uint32_t src) {
        uint32_t id;
        std::string err;
        if (!s.id_layout.make(IdParts{priority, dst, port, src}, id, &err)) throw py::value_error(err);
        return id;
      }, py::arg("priority"), py::arg("dst"), py::arg("port"), py::arg("src"))
      .def("parse_id", [](const Schema& s, uint32_t id) {
        const IdParts p = s.id_layout.parse(id);
        py::dict d;
        d["priority"] = p.priority;
        d["dst"] = p.dst;
        d["port"] = p.port;
        d["src"] = p.src;
        return d;
      });

  // --- codec ---------------------------------------------------------------------------
  m.def("encode", [](const Message& msg, const py::object& values, bool raw, bool strict) {
    std::vector<uint8_t> out;
    std::string err;
    EncodeOptions o;
    o.raw = raw;
    o.strict = strict;
    const Values v = values.is_none() ? Values{} : to_values(values.cast<py::dict>());
    if (!encode(msg, v, out, &err, o)) {
      throw CodecError(err);
    }
    return py::bytes(reinterpret_cast<const char*>(out.data()), out.size());
  }, py::arg("message"), py::arg("values") = py::none(), py::kw_only(), py::arg("raw") = false,
     py::arg("strict") = true);

  m.def("decode", [](const Message& msg, const py::bytes& data, bool raw, bool labels, bool strict) {
    const std::string s = data;
    Values v;
    std::string err;
    DecodeOptions o;
    o.raw = raw;
    o.labels = labels;
    o.strict = strict;
    if (!decode(msg, reinterpret_cast<const uint8_t*>(s.data()), s.size(), v, &err, o)) {
      throw CodecError(err);
    }
    return from_values(msg, v);
  }, py::arg("message"), py::arg("data"), py::kw_only(), py::arg("raw") = false, py::arg("labels") = true,
     py::arg("strict") = true);

  // --- frames and links ----------------------------------------------------------------
  py::class_<Frame>(m, "Frame")
      .def(py::init(&make_frame), py::arg("id"), py::arg("data") = py::bytes(), py::arg("extended") = true)
      .def_readwrite("id", &Frame::id)
      .def_readwrite("extended", &Frame::extended)
      .def_property_readonly("data", &frame_bytes)
      .def_readonly("rx_ns", &Frame::rx_ns)
      .def("__repr__", [](const Frame& f) { return "<Frame " + to_cansend(f) + ">"; })
      .def("__str__", &to_cansend)
      .def_static("parse", [](const std::string& text) {
        Frame f;
        std::string err;
        if (!from_cansend(text, f, &err)) throw py::value_error(err + ": " + text);
        return f;
      });

  py::class_<Link>(m, "Link")
      .def("send", &Link::send, py::call_guard<py::gil_scoped_release>())
      .def("recv", [](Link& l, int timeout_ms) -> py::object {
        Frame f;
        RecvStatus st;
        {
          py::gil_scoped_release nogil;
          st = l.recv(f, timeout_ms);
        }
        if (st == RecvStatus::Closed) {
          PyErr_SetString(PyExc_EOFError, "link closed");
          throw py::error_already_set();
        }
        return st == RecvStatus::Ok ? py::cast(f) : py::object(py::none());
      }, py::arg("timeout_ms") = 0)
      .def("set_filters", [](Link& l, const py::object& filters) { return l.set_filters(to_filters(filters)); })
      .def_property_readonly("stats", [](const Link& l) { return stats_dict(l.stats()); })
      .def("close", &Link::close);

  py::class_<TestBus>(m, "TestBus")
      .def(py::init<size_t>(), py::arg("rx_queue") = 4096)
      .def("connect", &TestBus::connect, py::keep_alive<0, 1>())
      .def("set_tap", &TestBus::set_tap)
      .def_property_readonly("frames_sent", &TestBus::frames_sent);

#ifdef __linux__
  m.def("open_socketcan", [](const std::string& iface, const py::object& filters, bool receive_own) {
    SocketCanOptions o;
    o.filters = to_filters(filters);
    o.receive_own = receive_own;
    std::string err;
    std::unique_ptr<Link> l = SocketCanLink::open(iface, o, &err);
    if (!l) throw py::value_error(err);
    return l;
  }, py::arg("interface"), py::arg("filters") = py::none(), py::arg("receive_own") = false);
#endif

  // --- ISO-TP --------------------------------------------------------------------------
  py::class_<IsoTpChannel>(m, "IsoTpChannel")
      .def(py::init([](Link& link, const Schema& schema, uint32_t local, uint32_t peer, uint8_t block_size,
                       uint8_t st_min, int timeout_ms, uint32_t max_rx) {
             IsoTpConfig c;
             c.block_size = block_size;
             c.st_min = st_min;
             c.timeout_ms = timeout_ms;
             c.max_rx = max_rx;
             return std::make_unique<IsoTpChannel>(link, schema, local, peer, c);
           }),
           py::arg("link"), py::arg("schema"), py::arg("local"), py::arg("peer"), py::arg("block_size") = 0,
           py::arg("st_min") = 0, py::arg("timeout_ms") = 1000, py::arg("max_rx") = 65536,
           py::keep_alive<1, 2>(), py::keep_alive<1, 3>())
      .def("send", [](IsoTpChannel& ch, const py::bytes& data, int timeout_ms) {
        const std::string s = data;
        const std::vector<uint8_t> payload(s.begin(), s.end());
        IsoTpResult r;
        {
          py::gil_scoped_release nogil;
          r = ch.send(payload, timeout_ms);
        }
        if (r != IsoTpResult::Done) {
          throw IsoTpError(to_string(r));
        }
      }, py::arg("data"), py::arg("timeout_ms") = 10000)
      .def("recv", [](IsoTpChannel& ch, int timeout_ms) {
        std::vector<uint8_t> payload;
        IsoTpResult r;
        {
          py::gil_scoped_release nogil;
          r = ch.recv(payload, timeout_ms);
        }
        if (r != IsoTpResult::Done) {
          throw IsoTpError(to_string(r));
        }
        return py::bytes(reinterpret_cast<const char*>(payload.data()), payload.size());
      }, py::arg("timeout_ms") = 1000)
      .def_property_readonly("tx_id", &IsoTpChannel::tx_id)
      .def_property_readonly("rx_id", &IsoTpChannel::rx_id);

  bind_host(m);
}
