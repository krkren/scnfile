// psb.cpp - reads and writes M2's PSB format (plain, unencrypted). See psb.hpp and
// docs/format.md.
//
// SPDX-License-Identifier: 0BSD

#include "psb.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <unordered_map>

#ifdef SCNFILE_HAVE_ZLIB
#include <zlib.h>
#endif

namespace scnpsb {

namespace {

// Value types (the first byte of a value)
enum : uint8_t {
	T_NULL = 0x01,
	T_FALSE = 0x02,
	T_TRUE = 0x03,
	T_INT0 = 0x04,          // 0x04 + n: an integer in n bytes (0..8), signed, little-endian
	T_INT8 = 0x0C,
	T_ARRAY1 = 0x0D,        // 0x0C + n: an integer array, its count in n bytes (1..8)
	T_ARRAY8 = 0x14,
	T_STRING1 = 0x15,       // 0x14 + n: a string, its index in n bytes (1..4)
	T_STRING4 = 0x18,
	T_RESOURCE1 = 0x19,     // 0x18 + n: a resource, its index in n bytes (1..4)
	T_RESOURCE4 = 0x1C,
	T_REAL0 = 0x1D,         // 0.0
	T_FLOAT = 0x1E,         // a 4-byte float
	T_DOUBLE = 0x1F,        // an 8-byte double
	T_LIST = 0x20,
	T_OBJECT = 0x21,
	T_EXTRA1 = 0x22,        // 0x21 + n: a resource in the extra chunks (version 4), index in n bytes
	T_EXTRA4 = 0x25,
};

const size_t HEADER_V2 = 0x28;
const size_t HEADER_V3 = 0x2C;
const size_t HEADER_V4 = 0x38;

// ------------------------------------------------------------------------
// Writing
// ------------------------------------------------------------------------

using Bytes = std::vector<uint8_t>;

void putUint(Bytes &out, uint64_t v, int n) {
	for (int k = 0; k < n; k++) out.push_back(uint8_t(v >> (8 * k)));
}

void putUint32At(Bytes &out, size_t pos, uint32_t v) {
	for (int k = 0; k < 4; k++) out[pos + k] = uint8_t(v >> (8 * k));
}

// Bytes needed for an unsigned value (at least 1)
int unsignedSize(uint64_t v) {
	int n = 1;
	while (n < 8 && (v >> (8 * n)) != 0) n++;
	return n;
}

// Bytes needed for a signed value (at least 1)
int signedSize(int64_t v) {
	for (int n = 1; n < 8; n++) {
		int64_t lo = -(int64_t(1) << (8 * n - 1)), hi = (int64_t(1) << (8 * n - 1)) - 1;
		if (v >= lo && v <= hi) return n;
	}
	return 8;
}

// An integer array: count size, count, entry size, entries
void putArray(Bytes &out, const std::vector<uint64_t> &values) {
	int cn = unsignedSize(values.size());
	out.push_back(uint8_t(0x0C + cn));
	putUint(out, values.size(), cn);
	uint64_t maxv = 0;
	for (uint64_t v : values) maxv = std::max(maxv, v);
	int en = unsignedSize(maxv);
	out.push_back(uint8_t(0x0C + en));
	for (uint64_t v : values) putUint(out, v, en);
}

class Writer {
public:
	explicit Writer(const WriteOptions &options) : options(options) {}

	std::vector<uint8_t> write(const Value &root) {
		collectNames(root, 0);
		std::sort(names.begin(), names.end());
		names.erase(std::unique(names.begin(), names.end()), names.end());
		for (size_t k = 0; k < names.size(); k++) nameIds[names[k]] = k;

		Bytes entries;
		putValue(entries, root, 0);

		Bytes nameSection;
		putNames(nameSection);

		std::vector<uint64_t> stringOffsets;
		Bytes stringData;
		for (const std::string &s : strings) {
			stringOffsets.push_back(stringData.size());
			stringData.insert(stringData.end(), s.begin(), s.end());
			stringData.push_back(0);
		}
		Bytes stringOffsetSection;
		putArray(stringOffsetSection, stringOffsets);

		std::vector<uint64_t> chunkOffsets, chunkLengths;
		Bytes chunkData;
		for (const std::string *c : chunks) {
			chunkOffsets.push_back(chunkData.size());
			chunkLengths.push_back(c->size());
			chunkData.insert(chunkData.end(), c->begin(), c->end());
		}
		Bytes chunkOffsetSection, chunkLengthSection;
		putArray(chunkOffsetSection, chunkOffsets);
		putArray(chunkLengthSection, chunkLengths);

		// header | names | entries | string offsets | strings | chunk offsets | chunk lengths | chunks
		Bytes out(HEADER_V2, 0);
		std::memcpy(out.data(), "PSB\0", 4);
		out[4] = 2; // version 2: no checksum, no extra chunks
		out[5] = 0;
		out[6] = 0; // not encrypted
		out[7] = 0;
		auto section = [&out](const Bytes &b) {
			size_t pos = out.size();
			out.insert(out.end(), b.begin(), b.end());
			if (out.size() > 0xFFFFFFFFu) throw Error("PSB: the file would be over 4 GB");
			return uint32_t(pos);
		};
		uint32_t offNames = section(nameSection);
		uint32_t offEntries = section(entries);
		uint32_t offStrings = section(stringOffsetSection);
		uint32_t offStringData = section(stringData);
		uint32_t offChunkOffsets = section(chunkOffsetSection);
		uint32_t offChunkLengths = section(chunkLengthSection);
		uint32_t offChunkData = section(chunkData);
		putUint32At(out, 0x08, uint32_t(HEADER_V2)); // where encryption would start: after the header
		putUint32At(out, 0x0C, offNames);
		putUint32At(out, 0x10, offStrings);
		putUint32At(out, 0x14, offStringData);
		putUint32At(out, 0x18, offChunkOffsets);
		putUint32At(out, 0x1C, offChunkLengths);
		putUint32At(out, 0x20, offChunkData);
		putUint32At(out, 0x24, offEntries);
		return out;
	}

private:
	const WriteOptions &options;
	std::vector<std::string> names;
	std::map<std::string, uint64_t> nameIds;
	std::vector<std::string> strings;
	std::unordered_map<std::string, uint64_t> stringIds;
	std::vector<const std::string *> chunks;
	std::unordered_map<std::string, uint64_t> chunkIds;

	static void checkDepth(int depth) {
		if (depth > 1000) throw Error("PSB: values nested over 1000 deep");
	}

	void collectNames(const Value &v, int depth) {
		checkDepth(depth);
		if (v.type == Value::Type::List && v.list) {
			for (const Value &c : *v.list) collectNames(c, depth + 1);
		} else if (v.type == Value::Type::Object && v.object) {
			for (const auto &m : *v.object) {
				if (m.first.find('\0') != std::string::npos) throw Error("PSB: a name can't hold a NUL character");
				names.push_back(m.first);
				collectNames(m.second, depth + 1);
			}
		}
	}

	void putIndex(Bytes &out, uint8_t base, uint64_t index) {
		int n = unsignedSize(index);
		if (n > 4) throw Error("PSB: over 4 billion strings or resources");
		out.push_back(uint8_t(base + n - 1));
		putUint(out, index, n);
	}

	void putValue(Bytes &out, const Value &v, int depth) {
		checkDepth(depth);
		switch (v.type) {
		case Value::Type::Null:
			out.push_back(T_NULL);
			break;
		case Value::Type::Bool:
			out.push_back(v.b ? T_TRUE : T_FALSE);
			break;
		case Value::Type::Int:
			if (v.i == 0) {
				out.push_back(T_INT0);
			} else {
				int n = signedSize(v.i);
				out.push_back(uint8_t(T_INT0 + n));
				putUint(out, uint64_t(v.i), n);
			}
			break;
		case Value::Type::Real: {
			uint64_t bits;
			std::memcpy(&bits, &v.d, 8);
			float f = float(v.d);
			if (bits == 0) {
				out.push_back(T_REAL0);
			} else if (options.compactReals && !std::isnan(v.d) && double(f) == v.d) {
				uint32_t fb;
				std::memcpy(&fb, &f, 4);
				out.push_back(T_FLOAT);
				putUint(out, fb, 4);
			} else {
				out.push_back(T_DOUBLE);
				putUint(out, bits, 8);
			}
			break;
		}
		case Value::Type::String: {
			auto it = stringIds.find(v.s);
			uint64_t id;
			if (it != stringIds.end()) {
				id = it->second;
			} else {
				id = strings.size();
				strings.push_back(v.s);
				stringIds.emplace(v.s, id);
			}
			putIndex(out, T_STRING1, id);
			break;
		}
		case Value::Type::Resource: {
			auto it = chunkIds.find(v.s);
			uint64_t id;
			if (it != chunkIds.end()) {
				id = it->second;
			} else {
				id = chunks.size();
				auto ins = chunkIds.emplace(v.s, id);
				chunks.push_back(&ins.first->first);
			}
			putIndex(out, T_RESOURCE1, id);
			break;
		}
		case Value::Type::List: {
			std::vector<uint64_t> offsets;
			Bytes data;
			if (v.list) {
				for (const Value &c : *v.list) {
					offsets.push_back(data.size());
					putValue(data, c, depth + 1);
				}
			}
			out.push_back(T_LIST);
			putArray(out, offsets);
			out.insert(out.end(), data.begin(), data.end());
			break;
		}
		case Value::Type::Object: {
			// members in name order
			std::vector<std::pair<uint64_t, const Value *>> members;
			if (v.object) {
				for (const auto &m : *v.object) members.emplace_back(nameIds.at(m.first), &m.second);
			}
			std::sort(members.begin(), members.end(),
					  [](const auto &a, const auto &b) { return a.first < b.first; });
			for (size_t k = 1; k < members.size(); k++) {
				if (members[k].first == members[k - 1].first) {
					throw Error("PSB: an object with two members named \"" + names[members[k].first] + "\"");
				}
			}
			std::vector<uint64_t> ids, offsets;
			Bytes data;
			for (const auto &m : members) {
				ids.push_back(m.first);
				offsets.push_back(data.size());
				putValue(data, *m.second, depth + 1);
			}
			out.push_back(T_OBJECT);
			putArray(out, ids);
			putArray(out, offsets);
			out.insert(out.end(), data.begin(), data.end());
			break;
		}
		}
	}

	// The names as a double-array trie over their UTF-8 bytes: base ("charset"), check
	// ("names data") and, per name, the node its terminating 0 leads to. A node's child for
	// byte c is base[node] + c; check[child] is node; the root is node 0
	void putNames(Bytes &out) {
		struct TrieNode {
			std::map<int, size_t> children; // byte (0: the end of a name) -> node
			int64_t nameId = -1;
		};
		std::vector<TrieNode> trie(1);
		for (size_t id = 0; id < names.size(); id++) {
			size_t node = 0;
			const std::string &name = names[id];
			for (size_t k = 0; k <= name.size(); k++) {
				int c = k < name.size() ? uint8_t(name[k]) : 0;
				auto it = trie[node].children.find(c);
				if (it == trie[node].children.end()) {
					trie.push_back(TrieNode());
					size_t child = trie.size() - 1;
					trie[node].children[c] = child;
					node = child;
				} else {
					node = it->second;
				}
			}
			trie[node].nameId = int64_t(id);
		}

		std::vector<uint64_t> base(1, 0), check(1, 0), indexes(names.size(), 0);
		std::vector<bool> used(1, true); // the root's slot
		size_t firstFree = 1;
		std::vector<std::pair<size_t, size_t>> queue; // trie node, its index
		queue.emplace_back(0, 0);
		for (size_t q = 0; q < queue.size(); q++) {
			size_t tnode = queue[q].first, index = queue[q].second;
			const TrieNode &t = trie[tnode];
			if (t.nameId >= 0) indexes[size_t(t.nameId)] = index;
			if (t.children.empty()) continue;
			while (firstFree < used.size() && used[firstFree]) firstFree++;
			int minc = t.children.begin()->first;
			// the smallest base that puts every child in a free slot past the root
			uint64_t b = firstFree > size_t(minc) ? firstFree - minc : 1;
			if (b < 1) b = 1;
			for (;; b++) {
				bool ok = true;
				for (const auto &c : t.children) {
					size_t slot = b + c.first;
					if (slot < used.size() && used[slot]) { ok = false; break; }
				}
				if (ok) break;
			}
			base[index] = b;
			for (const auto &c : t.children) {
				size_t slot = b + c.first;
				if (slot >= used.size()) {
					used.resize(slot + 1, false);
					base.resize(slot + 1, 0);
					check.resize(slot + 1, 0);
				}
				used[slot] = true;
				check[slot] = index;
				queue.emplace_back(c.second, slot);
			}
		}
		putArray(out, base);
		putArray(out, check);
		putArray(out, indexes);
	}
};

// ------------------------------------------------------------------------
// Reading
// ------------------------------------------------------------------------

class Reader {
public:
	Reader(const uint8_t *data, size_t size) : data(data), size(size) {}

	Value read() {
		if (size < HEADER_V2 || std::memcmp(data, "PSB\0", 4) != 0) throw Error("PSB: not a PSB file");
		version = u16(4);
		uint16_t encrypt = u16(6);
		if (version < 2 || version > 4) throw Error("PSB: version " + std::to_string(version) + " isn't supported");
		if (encrypt != 0) throw Error("PSB: an encrypted PSB header isn't supported");
		size_t header = version >= 4 ? HEADER_V4 : version == 3 ? HEADER_V3 : HEADER_V2;
		if (size < header) throw Error("PSB: the header is cut short");

		size_t offNames = u32(0x0C), offStrings = u32(0x10), offStringData = u32(0x14);
		size_t offChunkOffsets = u32(0x18), offChunkLengths = u32(0x1C);
		offChunkData = u32(0x20);
		size_t offEntries = u32(0x24);

		// names
		size_t pos = offNames;
		std::vector<uint64_t> base = array(pos), check = array(pos), indexes = array(pos);
		names.reserve(indexes.size());
		for (uint64_t index : indexes) names.push_back(decodeName(base, check, index));

		// strings
		pos = offStrings;
		stringOffsets = array(pos);
		stringData = offStringData;
		strings.assign(stringOffsets.size(), std::string());
		stringDone.assign(stringOffsets.size(), false);

		// resources
		pos = offChunkOffsets;
		chunkOffsets = array(pos);
		pos = offChunkLengths;
		chunkLengths = array(pos);
		if (chunkOffsets.size() != chunkLengths.size()) throw Error("PSB: resource tables of different sizes");
		if (version >= 4) {
			size_t offExtraOffsets = u32(0x2C), offExtraLengths = u32(0x30);
			offExtraData = u32(0x34);
			if (offExtraOffsets != 0) {
				pos = offExtraOffsets;
				extraOffsets = array(pos);
				pos = offExtraLengths;
				extraLengths = array(pos);
				if (extraOffsets.size() != extraLengths.size()) throw Error("PSB: extra resource tables of different sizes");
			}
		}

		pos = offEntries;
		return value(pos, 0);
	}

private:
	const uint8_t *data;
	size_t size;
	uint16_t version = 0;
	std::vector<std::string> names;
	std::vector<uint64_t> stringOffsets;
	size_t stringData = 0;
	std::vector<std::string> strings;
	std::vector<bool> stringDone;
	std::vector<uint64_t> chunkOffsets, chunkLengths;
	size_t offChunkData = 0;
	std::vector<uint64_t> extraOffsets, extraLengths;
	size_t offExtraData = 0;
	size_t valuesRead = 0;

	void need(size_t pos, size_t n) const {
		if (pos > size || n > size - pos) throw Error("PSB: the file is cut short or corrupt");
	}
	uint16_t u16(size_t pos) const {
		need(pos, 2);
		return uint16_t(data[pos] | (data[pos + 1] << 8));
	}
	uint32_t u32(size_t pos) const {
		need(pos, 4);
		return uint32_t(data[pos]) | (uint32_t(data[pos + 1]) << 8) | (uint32_t(data[pos + 2]) << 16) |
			   (uint32_t(data[pos + 3]) << 24);
	}
	uint64_t uintN(size_t pos, int n) const {
		need(pos, size_t(n));
		uint64_t v = 0;
		for (int k = 0; k < n; k++) v |= uint64_t(data[pos + k]) << (8 * k);
		return v;
	}

	std::vector<uint64_t> array(size_t &pos) {
		need(pos, 1);
		uint8_t t = data[pos++];
		if (t < T_ARRAY1 || t > T_ARRAY8) throw Error("PSB: an integer array was expected");
		int cn = t - 0x0C;
		uint64_t count = uintN(pos, cn);
		pos += cn;
		need(pos, 1);
		uint8_t et = data[pos++];
		if (et < T_ARRAY1 || et > T_ARRAY8) throw Error("PSB: an integer array was expected");
		int en = et - 0x0C;
		if (count > (size - pos) / size_t(en)) throw Error("PSB: the file is cut short or corrupt");
		std::vector<uint64_t> v(static_cast<size_t>(count));
		for (size_t k = 0; k < v.size(); k++) {
			v[k] = uintN(pos, en);
			pos += en;
		}
		return v;
	}

	std::string decodeName(const std::vector<uint64_t> &base, const std::vector<uint64_t> &check, uint64_t index) {
		if (index >= check.size()) throw Error("PSB: a name is corrupt");
		std::string reversed;
		uint64_t node = check[size_t(index)];
		size_t steps = 0;
		while (node != 0) {
			if (node >= check.size() || ++steps > check.size()) throw Error("PSB: a name is corrupt");
			uint64_t parent = check[size_t(node)];
			if (parent >= base.size() || node < base[size_t(parent)] || node - base[size_t(parent)] > 0xFF) {
				throw Error("PSB: a name is corrupt");
			}
			reversed.push_back(char(node - base[size_t(parent)]));
			node = parent;
		}
		return std::string(reversed.rbegin(), reversed.rend());
	}

	const std::string &string(uint64_t id) {
		if (id >= stringOffsets.size()) throw Error("PSB: a string index is out of range");
		if (!stringDone[size_t(id)]) {
			size_t start = stringData + size_t(stringOffsets[size_t(id)]);
			if (start < stringData || start >= size) throw Error("PSB: a string is out of range");
			const uint8_t *end = static_cast<const uint8_t *>(std::memchr(data + start, 0, size - start));
			if (!end) throw Error("PSB: a string isn't terminated");
			strings[size_t(id)].assign(reinterpret_cast<const char *>(data + start), size_t(end - (data + start)));
			stringDone[size_t(id)] = true;
		}
		return strings[size_t(id)];
	}

	std::string chunk(const std::vector<uint64_t> &offsets, const std::vector<uint64_t> &lengths, size_t start,
					  uint64_t id) {
		if (id >= offsets.size()) throw Error("PSB: a resource index is out of range");
		uint64_t off = offsets[size_t(id)], len = lengths[size_t(id)];
		if (off > size || len > size || start > size - off || len > size - start - off) {
			throw Error("PSB: a resource is out of range");
		}
		return std::string(reinterpret_cast<const char *>(data + start + off), size_t(len));
	}

	Value value(size_t pos, int depth) {
		if (depth > 1000) throw Error("PSB: values nested over 1000 deep");
		// a file whose offsets share values could expand without end
		if (++valuesRead > 50000000) throw Error("PSB: too many values");
		need(pos, 1);
		uint8_t t = data[pos++];
		switch (t) {
		case T_NULL:
			return Value();
		case T_FALSE:
			return Value::makeBool(false);
		case T_TRUE:
			return Value::makeBool(true);
		case T_REAL0:
			return Value::makeReal(0.0);
		case T_FLOAT: {
			uint32_t b = uint32_t(uintN(pos, 4));
			float f;
			std::memcpy(&f, &b, 4);
			return Value::makeReal(double(f));
		}
		case T_DOUBLE: {
			uint64_t b = uintN(pos, 8);
			double d;
			std::memcpy(&d, &b, 8);
			return Value::makeReal(d);
		}
		case T_LIST: {
			std::vector<uint64_t> offsets = array(pos);
			Value v = Value::makeList();
			v.list->reserve(offsets.size());
			for (uint64_t off : offsets) {
				if (off > size - pos) throw Error("PSB: a list item is out of range");
				v.list->push_back(value(pos + size_t(off), depth + 1));
			}
			return v;
		}
		case T_OBJECT: {
			std::vector<uint64_t> ids = array(pos);
			std::vector<uint64_t> offsets = array(pos);
			if (ids.size() != offsets.size()) throw Error("PSB: an object's names and values don't match");
			Value v = Value::makeObject();
			v.object->reserve(ids.size());
			for (size_t k = 0; k < ids.size(); k++) {
				if (ids[k] >= names.size()) throw Error("PSB: a name index is out of range");
				if (offsets[k] > size - pos) throw Error("PSB: an object member is out of range");
				v.object->emplace_back(names[size_t(ids[k])], value(pos + size_t(offsets[k]), depth + 1));
			}
			return v;
		}
		}
		if (t >= T_INT0 && t <= T_INT8) {
			int n = t - T_INT0;
			if (n == 0) return Value::makeInt(0);
			uint64_t u = uintN(pos, n);
			if (n < 8 && (u >> (8 * n - 1)) & 1) u |= ~uint64_t(0) << (8 * n); // sign extension
			return Value::makeInt(int64_t(u));
		}
		if (t >= T_STRING1 && t <= T_STRING4) {
			return Value::makeString(string(uintN(pos, t - T_STRING1 + 1)));
		}
		if (t >= T_RESOURCE1 && t <= T_RESOURCE4) {
			return Value::makeResource(chunk(chunkOffsets, chunkLengths, offChunkData, uintN(pos, t - T_RESOURCE1 + 1)));
		}
		if (t >= T_EXTRA1 && t <= T_EXTRA4 && version >= 4) {
			return Value::makeResource(chunk(extraOffsets, extraLengths, offExtraData, uintN(pos, t - T_EXTRA1 + 1)));
		}
		if (t >= T_ARRAY1 && t <= T_ARRAY8) {
			// an integer array as a value: a list of integers
			pos--;
			std::vector<uint64_t> ints = array(pos);
			Value v = Value::makeList();
			for (uint64_t x : ints) v.list->push_back(Value::makeInt(int64_t(x)));
			return v;
		}
		throw Error("PSB: unknown value type 0x" + std::string(1, "0123456789abcdef"[t >> 4]) +
					std::string(1, "0123456789abcdef"[t & 15]));
	}
};

bool isMdf(const uint8_t *data, size_t size) {
	return size >= 8 && std::memcmp(data, "mdf\0", 4) == 0;
}

} // namespace

std::vector<uint8_t> write(const Value &root, const WriteOptions &options) {
	return Writer(options).write(root);
}

Value read(const uint8_t *data, size_t size) {
	if (isMdf(data, size)) {
#ifdef SCNFILE_HAVE_ZLIB
		uint32_t expected = uint32_t(data[4]) | (uint32_t(data[5]) << 8) | (uint32_t(data[6]) << 16) |
							(uint32_t(data[7]) << 24);
		if (expected > 0x7FFFFFFFu) throw Error("PSB: an mdf file too large to expand");
		std::vector<uint8_t> out(expected ? expected : 1);
		uLongf outLen = expected;
		int r = uncompress(out.data(), &outLen, data + 8, uLong(size - 8));
		if (r != Z_OK || outLen != expected) throw Error("PSB: the mdf file's zlib data is corrupt");
		if (isMdf(out.data(), outLen)) throw Error("PSB: an mdf file inside an mdf file");
		return read(out.data(), outLen);
#else
		throw Error("PSB: this build can't expand mdf (zlib) files");
#endif
	}
	return Reader(data, size).read();
}

bool looksLikePsb(const uint8_t *data, size_t size) {
	return (size >= 4 && std::memcmp(data, "PSB\0", 4) == 0) || isMdf(data, size);
}

bool equal(const Value &a, const Value &b) {
	if (a.type != b.type) return false;
	switch (a.type) {
	case Value::Type::Null:
		return true;
	case Value::Type::Bool:
		return a.b == b.b;
	case Value::Type::Int:
		return a.i == b.i;
	case Value::Type::Real:
		return std::memcmp(&a.d, &b.d, sizeof(double)) == 0 || (std::isnan(a.d) && std::isnan(b.d));
	case Value::Type::String:
	case Value::Type::Resource:
		return a.s == b.s;
	case Value::Type::List: {
		size_t na = a.list ? a.list->size() : 0, nb = b.list ? b.list->size() : 0;
		if (na != nb) return false;
		for (size_t k = 0; k < na; k++) {
			if (!equal((*a.list)[k], (*b.list)[k])) return false;
		}
		return true;
	}
	case Value::Type::Object: {
		size_t na = a.object ? a.object->size() : 0, nb = b.object ? b.object->size() : 0;
		if (na != nb) return false;
		std::map<std::string, const Value *> bm;
		if (b.object) {
			for (const auto &m : *b.object) bm[m.first] = &m.second;
		}
		if (bm.size() != nb) return false;
		if (a.object) {
			for (const auto &m : *a.object) {
				auto it = bm.find(m.first);
				if (it == bm.end() || !equal(m.second, *it->second)) return false;
			}
		}
		return true;
	}
	}
	return false;
}

} // namespace scnpsb
