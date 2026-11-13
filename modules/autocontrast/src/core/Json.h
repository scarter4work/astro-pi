// AutoContrast core (PCL-free): JSON type and Python-compatible serialisation.
#pragma once

#include <nlohmann/json.hpp>

#include <string>

namespace ac {

// Insertion-ordered: the Python reference writes dicts in insertion order (session.json,
// fingerprint records), and byte-equal output needs the same order. (nlohmann::json would
// sort the keys.)
using Json = nlohmann::ordered_json;

// Exactly Python's json.dumps(obj) (indent < 0) or json.dumps(obj, indent=indent) (indent >= 0):
// separators ", " and ": " (with indent: "," + newline, ": "), ensure_ascii=True (\uXXXX, surrogate
// pairs above the BMP), floats as Python's repr (shortest round trip; "1.0" for integral values,
// exponent form such as 1e-05 / 1.5e+16 where Python uses it), and the literals Infinity,
// -Infinity and NaN for non-finite doubles. Throws std::runtime_error on invalid UTF-8.
std::string dump_python(const Json& value, int indent = -1);

} // namespace ac
