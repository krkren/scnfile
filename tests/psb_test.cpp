// psb_test.cpp - tests of the PSB codec (plugins/scnfile/psb.cpp): files written by hand from
// docs/format.md, round trips of every kind of value, random trees, and damaged files.
//
// SPDX-License-Identifier: 0BSD

#include "psb.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <random>
#include <string>
#include <vector>

#ifdef SCNFILE_HAVE_ZLIB
#include <zlib.h>
#endif

using scnpsb::Value;

static int failures = 0;
static int checks = 0;

#define CHECK(cond)                                                             \
	do {                                                                        \
		checks++;                                                               \
		if (!(cond)) {                                                          \
			failures++;                                                         \
			std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
		}                                                                       \
	} while (0)

static bool throwsError(const std::vector<uint8_t> &bytes) {
	try {
		scnpsb::read(bytes.data(), bytes.size());
	} catch (const scnpsb::Error &) {
		return true;
	}
	return false;
}

static Value roundTrip(const Value &v, scnpsb::WriteOptions options = scnpsb::WriteOptions()) {
	std::vector<uint8_t> bytes = scnpsb::write(v, options);
	return scnpsb::read(bytes.data(), bytes.size());
}

static Value obj(std::initializer_list<std::pair<std::string, Value>> members) {
	Value v = Value::makeObject();
	for (const auto &m : members) v.object->push_back(m);
	return v;
}

static Value list(std::initializer_list<Value> items) {
	Value v = Value::makeList();
	for (const auto &i : items) v.list->push_back(i);
	return v;
}

// ------------------------------------------------------------------------

// A file written out by hand from docs/format.md: { "\x01": ["x", 5, null] }
static void testHandWritten() {
	std::vector<uint8_t> f = {
		'P', 'S', 'B', 0, 2, 0, 0, 0,
		0x28, 0, 0, 0,   // where encryption would start
		0x28, 0, 0, 0,   // names
		0x4D, 0, 0, 0,   // string offsets
		0x51, 0, 0, 0,   // strings
		0x53, 0, 0, 0,   // resource offsets
		0x56, 0, 0, 0,   // resource lengths
		0x59, 0, 0, 0,   // resources
		0x38, 0, 0, 0,   // the root
		// 0x28 names: base [0,2,0], check [0,0,1], name 0's end at 2
		0x0D, 3, 0x0D, 0, 2, 0,
		0x0D, 3, 0x0D, 0, 0, 1,
		0x0D, 1, 0x0D, 2,
		// 0x38 the root: an object of name 0 -> a list
		0x21, 0x0D, 1, 0x0D, 0, 0x0D, 1, 0x0D, 0,
		0x20, 0x0D, 3, 0x0D, 0, 2, 4, 0x15, 0, 0x05, 5, 0x01,
		// 0x4D string offsets [0], 0x51 "x"
		0x0D, 1, 0x0D, 0,
		'x', 0,
		// 0x53 resource offsets [], 0x56 lengths []
		0x0D, 0, 0x0D,
		0x0D, 0, 0x0D,
	};
	CHECK(f.size() == 0x59);
	Value v = scnpsb::read(f.data(), f.size());
	Value expected = obj({ { std::string("\x01"), list({ Value::makeString("x"), Value::makeInt(5), Value() }) } });
	CHECK(scnpsb::equal(v, expected));
}

// Value encodings written by hand: integers sign-extend, floats, booleans
static void testHandWrittenValues() {
	auto withRoot = [](std::vector<uint8_t> value) {
		std::vector<uint8_t> f = {
			'P', 'S', 'B', 0, 2, 0, 0, 0, 0x28, 0, 0, 0,
			0x28, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
			// 0x28 names: none (base [0], check [0], indexes [])
			0x0D, 1, 0x0D, 0, 0x0D, 1, 0x0D, 0, 0x0D, 0, 0x0D,
			// 0x33 strings, resources: none
			0x0D, 0, 0x0D, 0x0D, 0, 0x0D, 0x0D, 0, 0x0D,
		};
		auto set = [&f](size_t at, uint32_t v) { for (int k = 0; k < 4; k++) f[at + k] = uint8_t(v >> (8 * k)); };
		set(0x10, 0x33); set(0x14, 0x3C); set(0x18, 0x36); set(0x1C, 0x39); set(0x20, 0x3C);
		set(0x24, uint32_t(f.size()));
		f.insert(f.end(), value.begin(), value.end());
		return scnpsb::read(f.data(), f.size());
	};
	CHECK(withRoot({ 0x05, 0xFF }).i == -1);
	CHECK(withRoot({ 0x06, 0x00, 0x80 }).i == -32768);
	CHECK(withRoot({ 0x06, 0xFF, 0x7F }).i == 32767);
	CHECK(withRoot({ 0x04 }).i == 0);
	CHECK(withRoot({ 0x0C, 0, 0, 0, 0, 0, 0, 0, 0x80 }).i == std::numeric_limits<int64_t>::min());
	CHECK(withRoot({ 0x1D }).d == 0.0);
	CHECK(withRoot({ 0x1E, 0, 0, 0xC0, 0x3F }).d == 1.5);
	CHECK(withRoot({ 0x1F, 0x9A, 0x99, 0x99, 0x99, 0x99, 0x99, 0xB9, 0x3F }).d == 0.1);
	CHECK(withRoot({ 0x02 }).type == Value::Type::Bool && !withRoot({ 0x02 }).b);
	CHECK(withRoot({ 0x03 }).b);
	CHECK(withRoot({ 0x01 }).type == Value::Type::Null);
	// an integer array used as a value reads as a list of integers
	Value ints = withRoot({ 0x0D, 2, 0x0E, 1, 0, 0, 1 });
	CHECK(ints.type == Value::Type::List && ints.list->size() == 2 && (*ints.list)[1].i == 256);
}

static void testIntegers() {
	std::vector<int64_t> values = { 0, 1, -1, 127, 128, -128, -129, 255, 256, 32767, 32768, -32768, -32769,
									(int64_t(1) << 31) - 1, int64_t(1) << 31, -(int64_t(1) << 31), (int64_t(1) << 40) + 7,
									std::numeric_limits<int64_t>::max(), std::numeric_limits<int64_t>::min() };
	for (int64_t x : values) {
		Value v = roundTrip(list({ Value::makeInt(x) }));
		CHECK((*v.list)[0].type == Value::Type::Int && (*v.list)[0].i == x);
	}
	// the smallest encoding: 0 is one byte, 127 two, 128 three
	CHECK(scnpsb::write(Value::makeInt(0)).size() + 1 == scnpsb::write(Value::makeInt(127)).size());
	CHECK(scnpsb::write(Value::makeInt(127)).size() + 1 == scnpsb::write(Value::makeInt(128)).size());
}

static void testReals() {
	std::vector<double> values = { 0.0, -0.0, 1.5, -2.25, 0.1, 1e300, -1e-300, 3.4028234663852886e38,
								   std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(),
								   std::numeric_limits<double>::denorm_min() };
	for (bool compact : { true, false }) {
		scnpsb::WriteOptions o;
		o.compactReals = compact;
		for (double x : values) {
			Value v = roundTrip(Value::makeReal(x), o);
			CHECK(v.type == Value::Type::Real && std::memcmp(&v.d, &x, 8) == 0);
		}
		Value nan = roundTrip(Value::makeReal(std::numeric_limits<double>::quiet_NaN()), o);
		CHECK(nan.type == Value::Type::Real && std::isnan(nan.d));
	}
	// 1.5 fits in a float, 0.1 doesn't
	CHECK(scnpsb::write(Value::makeReal(1.5)).size() + 4 == scnpsb::write(Value::makeReal(0.1)).size());
}

static void testStrings() {
	std::vector<std::string> values = { "", "a", "hello world", "日本語のテキスト", "【ai/???】",
										"emoji \xF0\x9F\x8E\x89", std::string(70000, 'z') };
	Value l = Value::makeList();
	for (const auto &s : values) l.list->push_back(Value::makeString(s));
	Value v = roundTrip(l);
	CHECK(scnpsb::equal(v, l));
	// a string used twice is stored once
	Value twice = list({ Value::makeString("same text here"), Value::makeString("same text here") });
	Value once = list({ Value::makeString("same text here"), Value::makeInt(0) });
	CHECK(scnpsb::write(twice).size() <= scnpsb::write(once).size() + 1);
	// 300 different strings need two-byte indexes
	Value many = Value::makeList();
	for (int k = 0; k < 300; k++) many.list->push_back(Value::makeString("s" + std::to_string(k)));
	CHECK(scnpsb::equal(roundTrip(many), many));
}

static void testResources() {
	std::string binary;
	for (int k = 0; k < 1000; k++) binary.push_back(char(k * 7));
	Value l = list({ Value::makeResource(""), Value::makeResource(binary), Value::makeResource(std::string("\0\0\1", 3)),
					 Value::makeResource(binary) });
	CHECK(scnpsb::equal(roundTrip(l), l));
}

static void testNames() {
	// shared prefixes, the empty name, bytes over 0x7F
	Value o = obj({ { "a", Value::makeInt(1) }, { "ab", Value::makeInt(2) }, { "abc", Value::makeInt(3) },
					{ "b", Value::makeInt(4) }, { "", Value::makeInt(5) }, { "名前", Value::makeInt(6) },
					{ "tagname", Value::makeInt(7) }, { "*cond", Value::makeInt(8) }, { "\x7F\xFF", Value::makeInt(9) } });
	CHECK(scnpsb::equal(roundTrip(o), o));
	// many names, and the same names in several objects
	Value big = Value::makeObject();
	for (int k = 0; k < 2000; k++) big.object->emplace_back("key" + std::to_string(k * 37 % 2003), Value::makeInt(k));
	Value both = list({ big, big, obj({ { "key0", Value() } }) });
	CHECK(scnpsb::equal(roundTrip(both), both));
	// members come back sorted by name
	Value sorted = roundTrip(obj({ { "z", Value() }, { "a", Value() }, { "m", Value() } }));
	CHECK((*sorted.object)[0].first == "a" && (*sorted.object)[2].first == "z");
}

static void testContainers() {
	CHECK(scnpsb::equal(roundTrip(Value::makeList()), Value::makeList()));
	CHECK(scnpsb::equal(roundTrip(Value::makeObject()), Value::makeObject()));
	Value nested = Value::makeList();
	Value *cur = &nested;
	for (int k = 0; k < 200; k++) {
		cur->list->push_back(Value::makeList());
		cur = &cur->list->back();
	}
	CHECK(scnpsb::equal(roundTrip(nested), nested));
	Value wide = Value::makeList();
	for (int k = 0; k < 70000; k++) wide.list->push_back(Value::makeInt(k % 300));
	CHECK(scnpsb::equal(roundTrip(wide), wide));
	// the root can be any value
	CHECK(roundTrip(Value::makeString("just a string")).s == "just a string");
	CHECK(roundTrip(Value()).type == Value::Type::Null);
}

static void testWriteErrors() {
	bool threw = false;
	try {
		scnpsb::write(obj({ { "dup", Value() }, { "dup", Value() } }));
	} catch (const scnpsb::Error &) {
		threw = true;
	}
	CHECK(threw);
	threw = false;
	try {
		scnpsb::write(obj({ { std::string("a\0b", 3), Value() } }));
	} catch (const scnpsb::Error &) {
		threw = true;
	}
	CHECK(threw);
	threw = false;
	Value deep = Value::makeList();
	Value *cur = &deep;
	for (int k = 0; k < 1100; k++) {
		cur->list->push_back(Value::makeList());
		cur = &cur->list->back();
	}
	try {
		scnpsb::write(deep);
	} catch (const scnpsb::Error &) {
		threw = true;
	}
	CHECK(threw);
}

// A compiled scene, roughly as SceneParser.tjs makes one
static Value sampleScene() {
	Value state = obj({ { "stage", obj({ { "imageFile", Value::makeString("bg01_01") }, { "disp", Value::makeInt(1) } }) },
						{ "characters", list({ obj({ { "name", Value::makeString("ai") }, { "xpos", Value::makeInt(-480) },
													 { "zoom", Value::makeReal(1.5) } }) }) } });
	Value scene = obj({
		{ "label", Value::makeString("*start") },
		{ "title", Value::makeString("Start") },
		{ "firstLine", Value::makeInt(0) },
		{ "spCount", Value::makeInt(1) },
		{ "texts", list({ list({ Value::makeString("ai"), Value(), Value::makeString("こんにちは"),
								 list({ obj({ { "name", Value::makeString("ai") }, { "voice", Value::makeInt(1) } }) }),
								 Value::makeInt(0x10), state }) }) },
		{ "lines", list({ list({ Value::makeInt(0), state, Value::makeInt(1), Value(), Value::makeInt(3), Value::makeInt(0), Value() }),
						  list({ Value::makeString("ai"), Value::makeString("standard"), Value::makeInt(0) }),
						  Value::makeInt(1), Value::makeString("bgm_BGM01") }) },
		{ "nexts", list({ obj({ { "type", Value::makeInt(0) }, { "storage", Value::makeString("s0001.ks") } }) }) },
	});
	return obj({ { "version", Value::makeInt(1) }, { "hash", Value::makeString("1:abc") },
				 { "scenes", list({ scene, scene }) } });
}

static void testScene() {
	Value s = sampleScene();
	CHECK(scnpsb::equal(roundTrip(s), s));
}

// Random trees, written and read back
static Value randomValue(std::mt19937 &rng, int depth) {
	int kind = std::uniform_int_distribution<int>(0, depth > 4 ? 6 : 8)(rng);
	switch (kind) {
	case 0: return Value();
	case 1: return Value::makeBool(rng() & 1);
	case 2: {
		int bits = std::uniform_int_distribution<int>(0, 63)(rng);
		int64_t x = int64_t(rng()) << 32 | rng();
		x >>= bits;
		return Value::makeInt(x);
	}
	case 3: {
		double d;
		uint64_t b = uint64_t(rng()) << 32 | rng();
		std::memcpy(&d, &b, 8);
		if (rng() & 1) d = double(int(rng() % 2000) - 1000) / 8;
		return Value::makeReal(d);
	}
	case 4:
	case 5: {
		std::string s;
		int n = std::uniform_int_distribution<int>(0, 12)(rng);
		static const char *parts[] = { "a", "b", "z", " ", "日", "本", "\xF0\x9F\x8E\x89", "%", "[", "\\" };
		for (int k = 0; k < n; k++) s += parts[rng() % 10];
		return Value::makeString(s);
	}
	case 6: {
		std::string s;
		int n = std::uniform_int_distribution<int>(0, 40)(rng);
		for (int k = 0; k < n; k++) s.push_back(char(rng()));
		return Value::makeResource(s);
	}
	case 7: {
		Value l = Value::makeList();
		int n = std::uniform_int_distribution<int>(0, 6)(rng);
		for (int k = 0; k < n; k++) l.list->push_back(randomValue(rng, depth + 1));
		return l;
	}
	default: {
		Value o = Value::makeObject();
		int n = std::uniform_int_distribution<int>(0, 6)(rng);
		for (int k = 0; k < n; k++) {
			std::string name = "n" + std::to_string(rng() % 50);
			bool dup = false;
			for (const auto &m : *o.object) dup = dup || m.first == name;
			if (!dup) o.object->emplace_back(name, randomValue(rng, depth + 1));
		}
		return o;
	}
	}
}

static void testRandomTrees() {
	std::mt19937 rng(12345);
	for (int k = 0; k < 2000; k++) {
		Value v = randomValue(rng, 0);
		scnpsb::WriteOptions o;
		o.compactReals = (k & 1) == 0;
		CHECK(scnpsb::equal(roundTrip(v, o), v));
	}
}

// Damaged files must be refused, never read out of bounds (run under -DSCNFILE_SANITIZE=ON)
static void testDamagedFiles() {
	std::vector<uint8_t> good = scnpsb::write(sampleScene());
	int refused = 0;
	for (size_t len = 0; len < good.size(); len++) {
		std::vector<uint8_t> cut(good.begin(), good.begin() + long(len));
		if (throwsError(cut)) refused++;
	}
	// a file cut anywhere is refused (everything has to be there: the resource tables end it)
	CHECK(refused == int(good.size()));

	std::mt19937 rng(99);
	for (int k = 0; k < 5000; k++) {
		std::vector<uint8_t> bad = good;
		int flips = 1 + int(rng() % 4);
		for (int f = 0; f < flips; f++) bad[rng() % bad.size()] = uint8_t(rng());
		try {
			scnpsb::read(bad.data(), bad.size());
		} catch (const scnpsb::Error &) {
		}
	}
	// not a PSB at all
	std::vector<uint8_t> junk = { 'n', 'o', 'p', 'e' };
	CHECK(throwsError(junk));
	CHECK(throwsError(std::vector<uint8_t>()));
	// an encrypted header is refused, not decrypted
	std::vector<uint8_t> enc = good;
	enc[6] = 1;
	CHECK(throwsError(enc));
	// other versions
	std::vector<uint8_t> v1 = good;
	v1[4] = 1;
	CHECK(throwsError(v1));
	CHECK(scnpsb::looksLikePsb(good.data(), good.size()) && !scnpsb::looksLikePsb(junk.data(), junk.size()));
}

static void testMdf() {
#ifdef SCNFILE_HAVE_ZLIB
	std::vector<uint8_t> psb = scnpsb::write(sampleScene());
	uLongf len = compressBound(uLong(psb.size()));
	std::vector<uint8_t> z(len);
	CHECK(compress(z.data(), &len, psb.data(), uLong(psb.size())) == Z_OK);
	std::vector<uint8_t> mdf = { 'm', 'd', 'f', 0 };
	for (int k = 0; k < 4; k++) mdf.push_back(uint8_t(psb.size() >> (8 * k)));
	mdf.insert(mdf.end(), z.begin(), z.begin() + long(len));
	Value v = scnpsb::read(mdf.data(), mdf.size());
	CHECK(scnpsb::equal(v, sampleScene()));
	// a wrong size is refused
	mdf[4] ^= 1;
	CHECK(throwsError(mdf));
#else
	std::printf("(no zlib: mdf files aren't tested)\n");
#endif
}

int main() {
	testHandWritten();
	testHandWrittenValues();
	testIntegers();
	testReals();
	testStrings();
	testResources();
	testNames();
	testContainers();
	testWriteErrors();
	testScene();
	testRandomTrees();
	testDamagedFiles();
	testMdf();
	std::printf("%d checks, %d failed\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
