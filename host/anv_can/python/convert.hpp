// Python <-> codec value conversion, shared by the anv_can and anv_host bindings.
#pragma once

#include <pybind11/pybind11.h>

#include "anv/codec.hpp"

namespace anv_py {

namespace py = pybind11;
using anv::FieldValue;
using anv::Field;
using anv::Message;
using anv::Scalar;
using anv::Values;

inline Scalar to_scalar(const py::handle& v) {
  if (py::isinstance<py::bool_>(v)) return v.cast<bool>();
  if (py::isinstance<py::int_>(v)) {
    const py::int_ i = py::reinterpret_borrow<py::int_>(v);
    if (i < py::int_(0)) return v.cast<int64_t>();
    return v.cast<uint64_t>();
  }
  if (py::isinstance<py::float_>(v)) return v.cast<double>();
  if (py::isinstance<py::str>(v)) return v.cast<std::string>();
  throw py::type_error("field values must be bool, int, float or str");
}

inline Values to_values(const py::dict& d) {
  Values out;
  for (const auto& kv : d) {
    const std::string name = py::str(kv.first);
    FieldValue fv;
    if (py::isinstance<py::list>(kv.second) || py::isinstance<py::tuple>(kv.second)) {
      for (const auto& item : kv.second) fv.push_back(to_scalar(item));
    } else {
      fv.push_back(to_scalar(kv.second));
    }
    out[name] = std::move(fv);
  }
  return out;
}

inline py::object from_scalar(const Scalar& s) {
  return std::visit([](const auto& x) -> py::object { return py::cast(x); }, s);
}

inline py::dict from_values(const Message& m, const Values& v) {
  py::dict d;
  for (const Field& f : m.fields) {  // schema order, like the Python codec
    auto it = v.find(f.name);
    if (it == v.end()) continue;
    if (f.count > 1) {
      py::list l;
      for (const Scalar& s : it->second) l.append(from_scalar(s));
      d[py::str(f.name)] = l;
    } else {
      d[py::str(f.name)] = from_scalar(it->second.front());
    }
  }
  return d;
}

}  // namespace anv_py
