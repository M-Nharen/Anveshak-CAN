// Message encode/decode by name, driven by the schema. Same bit rule as the generated C and the
// Python codec: the frame is one little-endian 64-bit integer and a field of n bits at bit s is
// (frame >> s) & mask(n). The tests check all three agree byte for byte.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <variant>
#include <vector>

#include "anv/link.hpp"
#include "anv/schema.hpp"

namespace anv {

// One element of a field. Unsigned integers decode as uint64_t, signed as int64_t, scaled
// values and floats as double, enum values (with labels on) as their label.
using Scalar = std::variant<bool, int64_t, uint64_t, double, std::string>;
using FieldValue = std::vector<Scalar>;  // one element, or `count` for arrays
using Values = std::map<std::string, FieldValue>;

struct EncodeOptions {
  bool raw = false;    // values are wire integers, not physical
  bool strict = true;  // refuse unknown fields and physical values outside min/max
};

struct DecodeOptions {
  bool raw = false;     // wire integers out
  bool labels = true;   // enum values as labels
  bool strict = true;   // check length and const fields
};

// values: field name -> value. Missing fields are 0 (or their const). An array given one
// element repeats it. Output is exactly msg.length bytes.
bool encode(const Message& msg, const Values& values, std::vector<uint8_t>& out, std::string* error,
            const EncodeOptions& options = {});
bool decode(const Message& msg, const uint8_t* data, size_t len, Values& out, std::string* error,
            const DecodeOptions& options = {});

// The whole frame: ID from the message's port and priority, data from encode().
bool encode_frame(const Schema& schema, const Message& msg, const Values& values, uint32_t src, uint32_t dst,
                  Frame& out, std::string* error, const EncodeOptions& options = {});

struct DecodedFrame {
  IdParts id;
  const Message* message = nullptr;  // nullptr: no schema describes this port for this sender
  Values values;                     // empty for `frame` messages (ISO-TP) and on error
  std::string error;
};

// node_type: the sender's type (or, for a command, the receiver's), when known.
DecodedFrame decode_frame(const Schema& schema, const Frame& frame, const NodeType* node_type,
                          const DecodeOptions& options = {});

// Helpers for C++ callers that know their field types.
inline FieldValue one(Scalar v) { return FieldValue{std::move(v)}; }
double as_double(const Scalar& v);  // bools 0/1, labels NaN
std::string to_string(const Scalar& v);

}  // namespace anv
