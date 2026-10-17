// The schema as the C++ host side sees it: loaded at run time from schema/generated/schema.json
// (written by `anv-schema gen`). Nothing about the bus is hard-coded here: ID widths, ports,
// layouts and enums all come from that file.
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace anv {

struct ValueType {
  std::string name;  // "u8", "i16", "bool", "f32", ...
  uint8_t bits = 0;
  bool is_signed = false;
  bool is_float = false;
  bool is_bool = false;
  static bool parse(const std::string& name, ValueType& out);
};

using EnumValues = std::vector<std::pair<std::string, int64_t>>;  // label -> number, file order

struct Field {
  std::string name;
  ValueType type;
  uint16_t start_bit = 0;
  uint16_t count = 1;
  double scale = 1.0;
  double offset = 0.0;
  std::string unit;
  std::optional<double> min, max;  // physical
  std::string enum_name;           // empty: no enum
  EnumValues enum_values;
  std::optional<uint64_t> const_value;  // raw

  bool is_scaled() const { return scale != 1.0 || offset != 0.0; }
  uint16_t element_start(uint16_t i) const { return static_cast<uint16_t>(start_bit + i * type.bits); }
  const std::string* label(int64_t value) const;
  std::optional<int64_t> enum_value(const std::string& label) const;
};

struct Rate {
  double def = 0, min = 0, max = 0;  // Hz
};

struct Message {
  std::string name;
  uint16_t port = 0;
  uint8_t priority = 0;
  std::string sender;  // node | host | leader | any
  std::string kind;    // stream | event | command | frame
  uint8_t length = 0;  // bytes
  uint16_t watchdog_ms = 0;
  std::string apply;   // now | sync
  std::optional<Rate> rate;
  std::string description;
  std::vector<Field> fields;
  const Field* field(const std::string& name) const;
};

struct Param {
  uint16_t index = 0;
  std::string name;
  ValueType type;
  std::optional<double> def, min, max;
  std::string unit, access, group;
  bool persist = true, disarmed_only = false, reboot_required = false;
};

struct Action {
  uint16_t index = 0;
  std::string name;
  std::optional<ValueType> arg;
  bool disarmed_only = false;
};

struct Event {
  uint16_t code = 0;
  std::string name, severity, description;
};

struct NodeType {
  std::string name;
  uint16_t type_id = 0;
  uint16_t version = 0;
  uint32_t wire_hash = 0;
  std::vector<std::pair<std::string, EnumValues>> enums;
  std::vector<Message> messages;
  std::vector<Param> params;
  std::vector<Action> actions;
  std::vector<Event> events;

  const Message* message(const std::string& name) const;
  const Message* message(uint16_t port) const;
  const Param* param(const std::string& name) const;
  const Param* param(uint16_t index) const;
  const Action* action(const std::string& name) const;
  const Event* event(uint16_t code) const;
  uint16_t schema_hash16() const { return static_cast<uint16_t>(wire_hash & 0xFFFFu); }
};

struct IdParts {
  uint32_t priority = 0, dst = 0, port = 0, src = 0;
};

// How a 29-bit ID splits into priority | dst | port | src (widths from protocol.yaml).
class IdLayout {
 public:
  struct Part {
    std::string name;
    uint8_t bits = 0, shift = 0;
    uint32_t max() const { return (1u << bits) - 1u; }
  };
  // False (and *error) if a value doesn't fit its field.
  bool make(const IdParts& parts, uint32_t& id, std::string* error = nullptr) const;
  // Masks values to their widths; for callers that already know they fit.
  uint32_t make(uint32_t priority, uint32_t dst, uint32_t port, uint32_t src) const;
  IdParts parse(uint32_t id) const;
  const Part& part(const std::string& name) const;  // priority, dst, port or src
  const std::vector<Part>& parts() const { return parts_; }

  bool set(std::vector<Part> parts, std::string* error);

 private:
  std::vector<Part> parts_;
  Part priority_, dst_, port_, src_;
};

struct NodeIds {
  uint32_t broadcast = 0, unassigned = 255, first = 1, last = 254;
};

class Schema {
 public:
  static std::shared_ptr<Schema> load_file(const std::string& path, std::string* error);
  static std::shared_ptr<Schema> load_json(const std::string& text, std::string* error);
  // $ANV_SCHEMA_JSON, else the schema.json of the source tree this library was built from.
  static std::string default_path();

  IdLayout id_layout;
  NodeIds node_ids;
  uint16_t protocol_last_port = 63;
  NodeType protocol;
  std::vector<NodeType> types;

  const NodeType* type(const std::string& name) const;  // "protocol" gives the protocol
  const NodeType* type_by_id(uint16_t type_id) const;
  // Protocol ports mean the same for everyone; the rest need the sending (or, for commands,
  // the receiving) node's type. nullptr when nothing describes the port.
  const Message* message_for(uint16_t port, const NodeType* node_type) const;
  uint32_t max_port() const { return id_layout.part("port").max(); }
};

}  // namespace anv
