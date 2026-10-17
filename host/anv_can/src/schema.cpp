#include "anv/schema.hpp"

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "json.hpp"

#ifndef ANV_SCHEMA_JSON_DEFAULT
#define ANV_SCHEMA_JSON_DEFAULT ""
#endif

namespace anv {

namespace {

constexpr int kBundleFormat = 1;

using json::Value;

// Reading helpers. A missing or mistyped key is an error: schema.json is generated, so a
// mismatch means the generator and this loader disagree, and guessing would decode garbage.
struct Reader {
  std::string error;

  bool fail(const std::string& what) {
    if (error.empty()) error = what;
    return false;
  }
  const Value* get(const Value& obj, const char* key, Value::Type type, const std::string& where) {
    const Value* v = obj.get(key);
    if (v == nullptr || v->type != type) {
      fail(where + ": missing or bad '" + key + "'");
      return nullptr;
    }
    return v;
  }
  bool str(const Value& obj, const char* key, std::string& out, const std::string& where) {
    const Value* v = get(obj, key, Value::String, where);
    if (v) out = v->str;
    return v != nullptr;
  }
  template <typename T>
  bool uint(const Value& obj, const char* key, T& out, const std::string& where) {
    const Value* v = get(obj, key, Value::Number, where);
    if (!v) return false;
    if (v->num < 0 || v->num != std::floor(v->num) || v->num > 4294967295.0) {
      return fail(where + ": '" + key + "' is not a small non-negative integer");
    }
    out = static_cast<T>(v->num);
    return true;
  }
  bool boolean(const Value& obj, const char* key, bool& out, const std::string& where) {
    const Value* v = get(obj, key, Value::Bool, where);
    if (v) out = v->b;
    return v != nullptr;
  }
  std::optional<double> opt_num(const Value& obj, const char* key) {
    const Value* v = obj.get(key);
    if (v == nullptr || v->type != Value::Number) return std::nullopt;
    return v->num;
  }
  std::string opt_str(const Value& obj, const char* key) {
    const Value* v = obj.get(key);
    return v != nullptr && v->type == Value::String ? v->str : std::string();
  }
  bool vtype(const Value& obj, const char* key, ValueType& out, const std::string& where) {
    std::string name;
    if (!str(obj, key, name, where)) return false;
    if (!ValueType::parse(name, out)) return fail(where + ": unknown type '" + name + "'");
    return true;
  }
  bool enum_values(const Value& obj, EnumValues& out, const std::string& where) {
    if (obj.type != Value::Object) return fail(where + ": enum is not an object");
    for (const std::string& k : obj.keys) {
      const Value& n = obj.obj.at(k);
      if (n.type != Value::Number) return fail(where + ": enum value " + k + " is not a number");
      out.emplace_back(k, static_cast<int64_t>(n.num));
    }
    return true;
  }

  bool field(const Value& v, Field& f, const std::string& where) {
    if (!str(v, "name", f.name, where)) return false;
    const std::string w = where + "." + f.name;
    if (!vtype(v, "type", f.type, w) || !uint(v, "start_bit", f.start_bit, w) || !uint(v, "count", f.count, w)) {
      return false;
    }
    f.scale = opt_num(v, "scale").value_or(1.0);
    f.offset = opt_num(v, "offset").value_or(0.0);
    f.unit = opt_str(v, "unit");
    f.min = opt_num(v, "min");
    f.max = opt_num(v, "max");
    if (const auto c = opt_num(v, "const")) f.const_value = static_cast<uint64_t>(*c);
    const Value* e = v.get("enum");
    if (e != nullptr && e->type == Value::Object) {
      const Value* name = e->get("name");
      const Value* values = e->get("values");
      if (values == nullptr) return fail(w + ": enum without values");
      f.enum_name = name != nullptr && name->type == Value::String ? name->str : std::string();
      if (!enum_values(*values, f.enum_values, w)) return false;
    }
    if (f.count == 0 || f.start_bit + f.count * f.type.bits > 64) return fail(w + ": doesn't fit in 64 bits");
    return true;
  }

  bool message(const Value& v, Message& m, const std::string& where) {
    if (!str(v, "name", m.name, where)) return false;
    const std::string w = where + "." + m.name;
    if (!uint(v, "port", m.port, w) || !uint(v, "priority", m.priority, w) || !str(v, "sender", m.sender, w) ||
        !str(v, "kind", m.kind, w) || !uint(v, "length", m.length, w) || !uint(v, "watchdog_ms", m.watchdog_ms, w) ||
        !str(v, "apply", m.apply, w)) {
      return false;
    }
    if (m.length > 8) return fail(w + ": longer than 8 bytes");
    m.description = opt_str(v, "description");
    const Value* r = v.get("rate");
    if (r != nullptr && r->type == Value::Object) {
      m.rate = Rate{opt_num(*r, "default").value_or(0), opt_num(*r, "min").value_or(0),
                    opt_num(*r, "max").value_or(0)};
    }
    const Value* fields = get(v, "fields", Value::Array, w);
    if (!fields) return false;
    for (const Value& fv : fields->arr) {
      Field f;
      if (!field(fv, f, w)) return false;
      m.fields.push_back(std::move(f));
    }
    return true;
  }

  bool node_type(const Value& v, NodeType& t, const std::string& where) {
    if (v.type != Value::Object || !str(v, "name", t.name, where)) return fail(where + ": not a type");
    const std::string& w = t.name;
    if (!uint(v, "type_id", t.type_id, w) || !uint(v, "version", t.version, w) ||
        !uint(v, "wire_hash", t.wire_hash, w)) {
      return false;
    }
    if (const Value* enums = get(v, "enums", Value::Object, w)) {
      for (const std::string& k : enums->keys) {
        EnumValues ev;
        if (!enum_values(enums->obj.at(k), ev, w + ".enums." + k)) return false;
        t.enums.emplace_back(k, std::move(ev));
      }
    } else {
      return false;
    }
    const Value* msgs = get(v, "messages", Value::Array, w);
    const Value* params = get(v, "params", Value::Array, w);
    const Value* actions = get(v, "actions", Value::Array, w);
    const Value* events = get(v, "events", Value::Array, w);
    if (!msgs || !params || !actions || !events) return false;
    for (const Value& mv : msgs->arr) {
      Message m;
      if (!message(mv, m, w)) return false;
      t.messages.push_back(std::move(m));
    }
    for (const Value& pv : params->arr) {
      Param p;
      if (!str(pv, "name", p.name, w) || !uint(pv, "index", p.index, w + "." + p.name) ||
          !vtype(pv, "type", p.type, w + "." + p.name)) {
        return false;
      }
      p.def = opt_num(pv, "default");
      p.min = opt_num(pv, "min");
      p.max = opt_num(pv, "max");
      p.unit = opt_str(pv, "unit");
      p.access = opt_str(pv, "access");
      p.group = opt_str(pv, "group");
      if (!boolean(pv, "persist", p.persist, w) || !boolean(pv, "disarmed_only", p.disarmed_only, w) ||
          !boolean(pv, "reboot_required", p.reboot_required, w)) {
        return false;
      }
      t.params.push_back(std::move(p));
    }
    for (const Value& av : actions->arr) {
      Action a;
      if (!str(av, "name", a.name, w) || !uint(av, "index", a.index, w + "." + a.name) ||
          !boolean(av, "disarmed_only", a.disarmed_only, w + "." + a.name)) {
        return false;
      }
      const std::string arg = opt_str(av, "arg");
      if (!arg.empty()) {
        ValueType vt;
        if (!ValueType::parse(arg, vt)) return fail(w + "." + a.name + ": unknown arg type");
        a.arg = vt;
      }
      t.actions.push_back(std::move(a));
    }
    for (const Value& ev : events->arr) {
      Event e;
      if (!str(ev, "name", e.name, w) || !uint(ev, "code", e.code, w + "." + e.name) ||
          !str(ev, "severity", e.severity, w + "." + e.name)) {
        return false;
      }
      e.description = opt_str(ev, "description");
      t.events.push_back(std::move(e));
    }
    return true;
  }
};

template <typename T, typename Pred>
const T* find_if(const std::vector<T>& v, Pred pred) {
  for (const T& x : v) {
    if (pred(x)) return &x;
  }
  return nullptr;
}

}  // namespace

bool ValueType::parse(const std::string& name, ValueType& out) {
  out = ValueType{};
  out.name = name;
  if (name == "bool") {
    out.bits = 1;
    out.is_bool = true;
    return true;
  }
  if (name == "f32") {
    out.bits = 32;
    out.is_signed = true;
    out.is_float = true;
    return true;
  }
  if (name.size() < 2 || (name[0] != 'u' && name[0] != 'i')) return false;
  for (size_t i = 1; i < name.size(); ++i) {
    if (name[i] < '0' || name[i] > '9') return false;
  }
  const int bits = std::atoi(name.c_str() + 1);
  out.is_signed = name[0] == 'i';
  if (bits < (out.is_signed ? 2 : 1) || bits > 64) return false;
  out.bits = static_cast<uint8_t>(bits);
  return true;
}

const std::string* Field::label(int64_t value) const {
  for (const auto& kv : enum_values) {
    if (kv.second == value) return &kv.first;
  }
  return nullptr;
}

std::optional<int64_t> Field::enum_value(const std::string& l) const {
  for (const auto& kv : enum_values) {
    if (kv.first == l) return kv.second;
  }
  return std::nullopt;
}

const Field* Message::field(const std::string& n) const {
  return find_if(fields, [&](const Field& f) { return f.name == n; });
}

const Message* NodeType::message(const std::string& n) const {
  return find_if(messages, [&](const Message& m) { return m.name == n; });
}
const Message* NodeType::message(uint16_t port) const {
  return find_if(messages, [&](const Message& m) { return m.port == port; });
}
const Param* NodeType::param(const std::string& n) const {
  return find_if(params, [&](const Param& p) { return p.name == n; });
}
const Param* NodeType::param(uint16_t index) const {
  return find_if(params, [&](const Param& p) { return p.index == index; });
}
const Action* NodeType::action(const std::string& n) const {
  return find_if(actions, [&](const Action& a) { return a.name == n; });
}
const Event* NodeType::event(uint16_t code) const {
  return find_if(events, [&](const Event& e) { return e.code == code; });
}

bool IdLayout::set(std::vector<Part> parts, std::string* error) {
  unsigned total = 0;
  for (const Part& p : parts) total += p.bits;
  auto find = [&](const char* n, Part& out) {
    for (const Part& p : parts) {
      if (p.name == n) {
        out = p;
        return p.bits > 0 && p.bits < 32;
      }
    }
    return false;
  };
  if (total != 29 || !find("priority", priority_) || !find("dst", dst_) || !find("port", port_) ||
      !find("src", src_)) {
    if (error) *error = "id_layout must have priority, dst, port and src adding up to 29 bits";
    return false;
  }
  parts_ = std::move(parts);
  return true;
}

uint32_t IdLayout::make(uint32_t priority, uint32_t dst, uint32_t port, uint32_t src) const {
  return (priority & priority_.max()) << priority_.shift | (dst & dst_.max()) << dst_.shift |
         (port & port_.max()) << port_.shift | (src & src_.max()) << src_.shift;
}

bool IdLayout::make(const IdParts& p, uint32_t& id, std::string* error) const {
  const std::pair<const Part*, uint32_t> checks[] = {
      {&priority_, p.priority}, {&dst_, p.dst}, {&port_, p.port}, {&src_, p.src}};
  for (const auto& c : checks) {
    if (c.second > c.first->max()) {
      if (error) *error = c.first->name + "=" + std::to_string(c.second) + " does not fit in " +
                          std::to_string(c.first->bits) + " bits";
      return false;
    }
  }
  id = make(p.priority, p.dst, p.port, p.src);
  return true;
}

IdParts IdLayout::parse(uint32_t id) const {
  IdParts p;
  p.priority = (id >> priority_.shift) & priority_.max();
  p.dst = (id >> dst_.shift) & dst_.max();
  p.port = (id >> port_.shift) & port_.max();
  p.src = (id >> src_.shift) & src_.max();
  return p;
}

const IdLayout::Part& IdLayout::part(const std::string& name) const {
  if (name == "priority") return priority_;
  if (name == "dst") return dst_;
  if (name == "port") return port_;
  return src_;
}

std::shared_ptr<Schema> Schema::load_file(const std::string& path, std::string* error) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    if (error) *error = "cannot read " + path;
    return nullptr;
  }
  std::stringstream ss;
  ss << in.rdbuf();
  return load_json(ss.str(), error);
}

std::shared_ptr<Schema> Schema::load_json(const std::string& text, std::string* error) {
  Value root;
  if (!json::Parser(text).parse(root) || root.type != Value::Object) {
    if (error) *error = "schema.json is not valid JSON";
    return nullptr;
  }
  Reader r;
  auto s = std::make_shared<Schema>();
  auto done = [&](bool ok) -> std::shared_ptr<Schema> {
    if (ok && r.error.empty()) return s;
    if (error) *error = r.error.empty() ? "bad schema.json" : r.error;
    return nullptr;
  };
  int format = 0;
  if (!r.uint(root, "format", format, "schema.json")) return done(false);
  if (format != kBundleFormat) {
    r.fail("schema.json format " + std::to_string(format) + ", this library reads " +
           std::to_string(kBundleFormat) + "; rebuild it");
    return done(false);
  }
  const Value* layout = r.get(root, "id_layout", Value::Array, "schema.json");
  if (!layout) return done(false);
  std::vector<IdLayout::Part> parts;
  for (const Value& p : layout->arr) {
    IdLayout::Part part;
    if (!r.str(p, "field", part.name, "id_layout") || !r.uint(p, "bits", part.bits, "id_layout") ||
        !r.uint(p, "shift", part.shift, "id_layout")) {
      return done(false);
    }
    parts.push_back(part);
  }
  std::string layout_error;
  if (!s->id_layout.set(parts, &layout_error)) return done(r.fail(layout_error));
  const Value* ids = r.get(root, "node_ids", Value::Object, "schema.json");
  if (!ids || !r.uint(*ids, "broadcast", s->node_ids.broadcast, "node_ids") ||
      !r.uint(*ids, "unassigned", s->node_ids.unassigned, "node_ids") ||
      !r.uint(*ids, "first", s->node_ids.first, "node_ids") || !r.uint(*ids, "last", s->node_ids.last, "node_ids") ||
      !r.uint(root, "protocol_last_port", s->protocol_last_port, "schema.json")) {
    return done(false);
  }
  const Value* proto = r.get(root, "protocol", Value::Object, "schema.json");
  const Value* types = r.get(root, "types", Value::Array, "schema.json");
  if (!proto || !types || !r.node_type(*proto, s->protocol, "protocol")) return done(false);
  for (const Value& tv : types->arr) {
    NodeType t;
    if (!r.node_type(tv, t, "types")) return done(false);
    s->types.push_back(std::move(t));
  }
  return done(true);
}

std::string Schema::default_path() {
  const char* env = std::getenv("ANV_SCHEMA_JSON");
  if (env != nullptr && *env != '\0') return env;
  return ANV_SCHEMA_JSON_DEFAULT;
}

const NodeType* Schema::type(const std::string& name) const {
  if (name == protocol.name) return &protocol;
  return find_if(types, [&](const NodeType& t) { return t.name == name; });
}

const NodeType* Schema::type_by_id(uint16_t type_id) const {
  if (type_id == protocol.type_id) return &protocol;
  return find_if(types, [&](const NodeType& t) { return t.type_id == type_id; });
}

const Message* Schema::message_for(uint16_t port, const NodeType* node_type) const {
  if (port <= protocol_last_port) return protocol.message(port);
  return node_type == nullptr ? nullptr : node_type->message(port);
}

}  // namespace anv
