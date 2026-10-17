#include "anv/codec.hpp"

#include <cmath>
#include <cstring>
#include <limits>
#include <sstream>

namespace anv {

namespace {

uint64_t mask(unsigned bits) { return bits >= 64 ? ~uint64_t{0} : (uint64_t{1} << bits) - 1u; }

bool fail(std::string* error, const std::string& what) {
  if (error) *error = what;
  return false;
}

std::string num_text(double v) {
  std::ostringstream ss;
  ss.precision(10);
  ss << v;
  return ss.str();
}

// An integer of either sign without needing 128-bit types (32-bit Raspberry Pi OS lacks them).
struct BigInt {
  bool neg = false;
  uint64_t mag = 0;
};

bool fits(const ValueType& t, const BigInt& n) {
  if (t.is_signed) {
    const uint64_t half = uint64_t{1} << (t.bits - 1);
    return n.neg ? n.mag <= half : n.mag < half;
  }
  return (!n.neg || n.mag == 0) && n.mag <= mask(t.bits);
}

bool from_double(double r, BigInt& out) {
  if (!std::isfinite(r) || std::fabs(r) >= 18446744073709551616.0) return false;
  out.neg = r < 0;
  out.mag = static_cast<uint64_t>(std::fabs(r));
  return true;
}

double to_double(const Scalar& v) {
  if (const bool* b = std::get_if<bool>(&v)) return *b ? 1.0 : 0.0;
  if (const int64_t* i = std::get_if<int64_t>(&v)) return static_cast<double>(*i);
  if (const uint64_t* u = std::get_if<uint64_t>(&v)) return static_cast<double>(*u);
  if (const double* d = std::get_if<double>(&v)) return *d;
  return std::numeric_limits<double>::quiet_NaN();
}

// Physical or raw value -> the field's wire bits. Mirrors anv_schema.codec._to_raw.
bool to_raw(const Field& f, const Scalar& value, const EncodeOptions& opt, uint64_t& bits, std::string* error) {
  const ValueType& t = f.type;
  if (t.is_float) {
    if (std::holds_alternative<std::string>(value)) return fail(error, f.name + ": needs a number");
    const float x = static_cast<float>(to_double(value));
    uint32_t u;
    std::memcpy(&u, &x, sizeof u);
    bits = u;
    return true;
  }
  BigInt n;
  if (const std::string* label = std::get_if<std::string>(&value)) {
    const auto v = f.enum_value(*label);
    if (!v) return fail(error, f.name + ": '" + *label + "' is not a label of this field");
    n = BigInt{*v < 0, *v < 0 ? uint64_t(0) - static_cast<uint64_t>(*v) : static_cast<uint64_t>(*v)};
  } else if (const bool* b = std::get_if<bool>(&value)) {
    n.mag = *b ? 1 : 0;
  } else if (opt.raw || !f.is_scaled()) {
    if (const int64_t* i = std::get_if<int64_t>(&value)) {
      n = BigInt{*i < 0, *i < 0 ? uint64_t(0) - static_cast<uint64_t>(*i) : static_cast<uint64_t>(*i)};
    } else if (const uint64_t* u = std::get_if<uint64_t>(&value)) {
      n.mag = *u;
    } else {
      double d = std::get<double>(value);
      if (std::isfinite(d) && d != std::floor(d)) {
        if (opt.raw) return fail(error, f.name + ": raw value " + num_text(d) + " must be an integer");
        d = std::nearbyint(d);  // round half to even, like Python's round()
      }
      if (!from_double(d, n)) return fail(error, f.name + ": " + num_text(d) + " doesn't fit " + t.name);
    }
  } else {
    const double d = to_double(value);
    if (opt.strict && ((f.min && d < *f.min) || (f.max && d > *f.max))) {
      std::string range = num_text(f.min ? *f.min : -INFINITY) + ".." + num_text(f.max ? *f.max : INFINITY);
      return fail(error, f.name + ": " + num_text(d) + " outside " + range + (f.unit.empty() ? "" : " " + f.unit));
    }
    if (!from_double(std::nearbyint((d - f.offset) / f.scale), n)) {
      return fail(error, f.name + ": " + num_text(d) + " doesn't fit " + t.name);
    }
  }
  if (!fits(t, n)) return fail(error, f.name + ": value doesn't fit " + t.name);
  bits = (n.neg ? uint64_t(0) - n.mag : n.mag) & mask(t.bits);
  return true;
}

Scalar from_raw(const Field& f, uint64_t bits, const DecodeOptions& opt) {
  const ValueType& t = f.type;
  if (t.is_float) {
    const uint32_t u = static_cast<uint32_t>(bits);
    float x;
    std::memcpy(&x, &u, sizeof x);
    return static_cast<double>(x);
  }
  if (t.is_bool) return bits != 0;
  int64_t n = 0;
  bool is_signed = t.is_signed;
  if (is_signed) {
    const uint64_t sign = uint64_t{1} << (t.bits - 1);
    n = static_cast<int64_t>((bits ^ sign) - sign);
  }
  if (opt.raw) return is_signed ? Scalar{n} : Scalar{bits};
  if (!f.enum_values.empty() && opt.labels) {
    // Values beyond int64 can't be enum members, so only look up those that fit.
    if (is_signed || bits <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
      if (const std::string* l = f.label(is_signed ? n : static_cast<int64_t>(bits))) return *l;
    }
  }
  if (f.is_scaled()) return (is_signed ? static_cast<double>(n) : static_cast<double>(bits)) * f.scale + f.offset;
  return is_signed ? Scalar{n} : Scalar{bits};
}

}  // namespace

bool encode(const Message& msg, const Values& values, std::vector<uint8_t>& out, std::string* error,
            const EncodeOptions& opt) {
  if (opt.strict) {
    for (const auto& kv : values) {
      if (msg.field(kv.first) == nullptr) return fail(error, msg.name + " has no field " + kv.first);
    }
  }
  uint64_t frame = 0;
  for (const Field& f : msg.fields) {
    FieldValue items;
    if (f.const_value) {
      items.assign(f.count, Scalar{*f.const_value});
    } else {
      auto it = values.find(f.name);
      if (it == values.end() || it->second.empty()) {
        items.assign(f.count, Scalar{int64_t{0}});
      } else if (it->second.size() == 1 && f.count > 1) {
        items.assign(f.count, it->second.front());
      } else {
        items = it->second;
      }
    }
    if (items.size() != f.count) {
      return fail(error, f.name + ": needs " + std::to_string(f.count) + " values, got " +
                             std::to_string(items.size()));
    }
    EncodeOptions item_opt = opt;
    if (f.const_value) item_opt.raw = true;
    for (uint16_t i = 0; i < f.count; ++i) {
      uint64_t bits = 0;
      if (!to_raw(f, items[i], item_opt, bits, error)) return false;
      frame |= bits << f.element_start(i);
    }
  }
  out.resize(msg.length);
  for (uint8_t i = 0; i < msg.length; ++i) out[i] = static_cast<uint8_t>(frame >> (8u * i));
  return true;
}

bool decode(const Message& msg, const uint8_t* data, size_t len, Values& out, std::string* error,
            const DecodeOptions& opt) {
  if (opt.strict && len < msg.length) {
    return fail(error, msg.name + ": " + std::to_string(len) + " bytes, needs " + std::to_string(msg.length));
  }
  uint64_t frame = 0;
  for (size_t i = 0; i < len && i < 8; ++i) frame |= uint64_t{data[i]} << (8u * i);
  out.clear();
  for (const Field& f : msg.fields) {
    const uint64_t m = mask(f.type.bits);
    FieldValue items;
    for (uint16_t i = 0; i < f.count; ++i) items.push_back(from_raw(f, (frame >> f.element_start(i)) & m, opt));
    if (opt.strict && f.const_value && ((frame >> f.start_bit) & m) != *f.const_value) {
      std::ostringstream ss;
      ss << msg.name << ": field " << f.name << " should be 0x" << std::hex << *f.const_value;
      return fail(error, ss.str());
    }
    out[f.name] = std::move(items);
  }
  return true;
}

bool encode_frame(const Schema& schema, const Message& msg, const Values& values, uint32_t src, uint32_t dst,
                  Frame& out, std::string* error, const EncodeOptions& options) {
  std::vector<uint8_t> data;
  if (!encode(msg, values, data, error, options)) return false;
  uint32_t id = 0;
  if (!schema.id_layout.make(IdParts{msg.priority, dst, msg.port, src}, id, error)) return false;
  out = Frame::make(id, data.data(), static_cast<uint8_t>(data.size()));
  return true;
}

DecodedFrame decode_frame(const Schema& schema, const Frame& frame, const NodeType* node_type,
                          const DecodeOptions& options) {
  DecodedFrame d;
  if (!frame.extended) {
    d.error = "not an extended (29-bit) frame";
    return d;
  }
  d.id = schema.id_layout.parse(frame.id);
  d.message = schema.message_for(static_cast<uint16_t>(d.id.port), node_type);
  if (d.message != nullptr && d.message->kind != "frame" &&
      !decode(*d.message, frame.data.data(), frame.len, d.values, &d.error, options)) {
    d.values.clear();
  }
  return d;
}

double as_double(const Scalar& v) { return to_double(v); }

std::string to_string(const Scalar& v) {
  if (const bool* b = std::get_if<bool>(&v)) return *b ? "true" : "false";
  if (const int64_t* i = std::get_if<int64_t>(&v)) return std::to_string(*i);
  if (const uint64_t* u = std::get_if<uint64_t>(&v)) return std::to_string(*u);
  if (const double* d = std::get_if<double>(&v)) return num_text(*d);
  return std::get<std::string>(v);
}

}  // namespace anv
