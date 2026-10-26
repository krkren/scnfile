// psb.hpp - reads and writes M2's PSB format (plain, unencrypted): the core of the scnfile
// plugin, with no engine dependencies, so it can be tested on its own.
//
// A PSB file holds a tree of values: null, booleans, integers, reals, strings, resources
// (binary chunks), lists and objects. docs/format.md describes the layout as this code reads
// and writes it.
//
// SPDX-License-Identifier: 0BSD

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace scnpsb {

// Raised for anything that isn't a PSB this code can read, or a value it can't write
class Error : public std::runtime_error {
public:
	using std::runtime_error::runtime_error;
};

struct Value;
using List = std::vector<Value>;
// An object's members, in any order (they're written sorted by name)
using Object = std::vector<std::pair<std::string, Value>>;

struct Value {
	enum class Type { Null, Bool, Int, Real, String, Resource, List, Object };

	Type type = Type::Null;
	bool b = false;
	int64_t i = 0;
	double d = 0;
	std::string s;                  // String: UTF-8. Resource: the bytes
	std::shared_ptr<scnpsb::List> list;
	std::shared_ptr<scnpsb::Object> object;

	Value() = default;
	static Value makeBool(bool v)                 { Value r; r.type = Type::Bool; r.b = v; return r; }
	static Value makeInt(int64_t v)               { Value r; r.type = Type::Int; r.i = v; return r; }
	static Value makeReal(double v)               { Value r; r.type = Type::Real; r.d = v; return r; }
	static Value makeString(std::string v)        { Value r; r.type = Type::String; r.s = std::move(v); return r; }
	static Value makeResource(std::string bytes)  { Value r; r.type = Type::Resource; r.s = std::move(bytes); return r; }
	static Value makeList()   { Value r; r.type = Type::List; r.list = std::make_shared<scnpsb::List>(); return r; }
	static Value makeObject() { Value r; r.type = Type::Object; r.object = std::make_shared<scnpsb::Object>(); return r; }
};

struct WriteOptions {
	// Use 4-byte floats for reals they hold exactly (false: always 8 bytes)
	bool compactReals = true;
};

// The tree as a plain PSB file. Raises Error for an object with two members of one name, or
// a name with a NUL in it
std::vector<uint8_t> write(const Value &root, const WriteOptions &options = WriteOptions());

// Reads a plain PSB file, or one wrapped in an "mdf" (zlib) container when built with zlib.
// Raises Error for anything else (an encrypted PSB, a truncated or corrupt file)
Value read(const uint8_t *data, size_t size);

// Whether the bytes start like something read() takes
bool looksLikePsb(const uint8_t *data, size_t size);

// Whether two trees are the same (reals compared bit for bit, objects regardless of order)
bool equal(const Value &a, const Value &b);

} // namespace scnpsb
