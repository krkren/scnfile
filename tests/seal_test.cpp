// seal_test.cpp - tests of scnfile's encrypted files and key slot (plugins/scnfile/seal.cpp)
//
// SPDX-License-Identifier: 0BSD

#include "psb.hpp"
#include "seal.hpp"

#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

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

template <typename F> static bool throwsError(F f) {
	try {
		f();
	} catch (const scnseal::Error &) {
		return true;
	}
	return false;
}

static scnseal::Key keyOf(uint8_t seed) {
	scnseal::Key k;
	for (size_t i = 0; i < k.size(); i++) k[i] = uint8_t(seed + i * 7 + 1);
	return k;
}

static std::vector<uint8_t> samplePsb(int n) {
	scnpsb::Value root = scnpsb::Value::makeObject();
	scnpsb::Value texts = scnpsb::Value::makeList();
	for (int i = 0; i < n; i++) texts.list->push_back(scnpsb::Value::makeString("line " + std::to_string(i) + " of the scene"));
	root.object->emplace_back("texts", texts);
	root.object->emplace_back("hash", scnpsb::Value::makeString("1:abc"));
	return scnpsb::write(root);
}

static void testRoundTrip() {
	scnseal::Key key = keyOf(1);
	for (int n : { 0, 1, 50, 2000 }) {
		std::vector<uint8_t> psb = samplePsb(n);
		for (bool compress : { true, false }) {
			std::vector<uint8_t> sealed = scnseal::seal(psb, key, compress);
			CHECK(scnseal::isSealed(sealed.data(), sealed.size()));
			CHECK(!scnseal::isSealed(psb.data(), psb.size()));
			CHECK(scnseal::unseal(sealed.data(), sealed.size(), key) == psb);
			// nothing of the scene shows through
			std::string s(sealed.begin(), sealed.end());
			CHECK(s.find("of the scene") == std::string::npos && s.find("PSB") == std::string::npos);
		}
	}
	// compressed is smaller for text
	std::vector<uint8_t> psb = samplePsb(2000);
	CHECK(scnseal::seal(psb, key, true).size() < scnseal::seal(psb, key, false).size() / 2);
}

static void testDeterministic() {
	scnseal::Key key = keyOf(2);
	std::vector<uint8_t> a = samplePsb(10), b = samplePsb(11);
	CHECK(scnseal::seal(a, key) == scnseal::seal(a, key));
	// different contents, different nonces
	std::vector<uint8_t> sa = scnseal::seal(a, key), sb = scnseal::seal(b, key);
	CHECK(std::memcmp(&sa[16], &sb[16], 24) != 0);
	// different keys, different files
	CHECK(scnseal::seal(a, key) != scnseal::seal(a, keyOf(3)));
}

static void testWrongKeyAndChanges() {
	scnseal::Key key = keyOf(4);
	std::vector<uint8_t> sealed = scnseal::seal(samplePsb(3), key);
	CHECK(throwsError([&] { scnseal::unseal(sealed.data(), sealed.size(), keyOf(5)); }));
	// any byte changed, anywhere, is refused
	int refused = 0;
	for (size_t i = 0; i < sealed.size(); i++) {
		std::vector<uint8_t> bad = sealed;
		bad[i] ^= 0x01;
		if (throwsError([&] { scnseal::unseal(bad.data(), bad.size(), key); })) refused++;
	}
	CHECK(refused == int(sealed.size()));
	// and any cut
	refused = 0;
	for (size_t len = 0; len < sealed.size(); len++) {
		if (throwsError([&] { scnseal::unseal(sealed.data(), len, key); })) refused++;
	}
	CHECK(refused == int(sealed.size()));
	// an extra byte
	std::vector<uint8_t> longer = sealed;
	longer.push_back(0);
	CHECK(throwsError([&] { scnseal::unseal(longer.data(), longer.size(), key); }));
	// not encrypted at all
	std::vector<uint8_t> psb = samplePsb(1);
	CHECK(throwsError([&] { scnseal::unseal(psb.data(), psb.size(), key); }));
}

static void testFormatFields() {
	scnseal::Key key = keyOf(6);
	std::vector<uint8_t> psb = samplePsb(5);
	std::vector<uint8_t> sealed = scnseal::seal(psb, key, false);
	CHECK(std::memcmp(sealed.data(), "KSCN", 4) == 0);
	CHECK(sealed[4] == 1 && sealed[5] == 0 && sealed[6] == 0 && sealed[7] == 0);
	CHECK(uint32_t(sealed[8] | (sealed[9] << 8) | (sealed[10] << 16) | (sealed[11] << 24)) == psb.size());
	CHECK(sealed.size() == scnseal::HEADER_SIZE + psb.size() + scnseal::TAG_SIZE);
	CHECK(scnseal::seal(psb, key, true)[5] == 1);
	// a later version, or flags this build doesn't know, are refused
	std::vector<uint8_t> v2 = sealed;
	v2[4] = 2;
	CHECK(throwsError([&] { scnseal::unseal(v2.data(), v2.size(), key); }));
	std::vector<uint8_t> f = sealed;
	f[5] = 2;
	CHECK(throwsError([&] { scnseal::unseal(f.data(), f.size(), key); }));
}

static void testSlot() {
	std::array<uint8_t, scnseal::SLOT_MARKER_SIZE> m = scnseal::slotMarker();
	CHECK(std::memcmp(m.data(), "KSCN-KEY-SLOT-v1", 16) == 0);

	std::mt19937 rng(7);
	std::vector<uint8_t> binary(5000);
	for (auto &b : binary) b = uint8_t(rng());
	CHECK(throwsError([&] { scnseal::findSlot(binary.data(), binary.size()); }));
	std::memcpy(&binary[1234], m.data(), m.size());
	std::memset(&binary[1234 + 16], 0, 32);
	CHECK(scnseal::findSlot(binary.data(), binary.size()) == 1234);
	scnseal::Key k;
	CHECK(!scnseal::readSlot(binary.data(), binary.size(), k));
	scnseal::Key key = keyOf(8);
	std::vector<uint8_t> before = binary;
	scnseal::writeSlot(binary, key);
	CHECK(scnseal::readSlot(binary.data(), binary.size(), k) && k == key);
	// only the key's 32 bytes changed
	size_t changed = 0;
	for (size_t i = 0; i < binary.size(); i++) changed += binary[i] != before[i];
	CHECK(changed > 0 && changed <= 32);
	for (size_t i = 0; i < binary.size(); i++) {
		if (binary[i] != before[i]) CHECK(i >= 1234 + 16 && i < 1234 + 48);
	}
	// a key can be replaced
	scnseal::writeSlot(binary, keyOf(9));
	CHECK(scnseal::readSlot(binary.data(), binary.size(), k) && k == keyOf(9));
	// two slots are ambiguous
	std::memcpy(&binary[3000], m.data(), m.size());
	CHECK(throwsError([&] { scnseal::findSlot(binary.data(), binary.size()); }));
	// a marker too close to the end has no room for a key
	std::vector<uint8_t> tail(40, 0);
	std::memcpy(&tail[30], m.data(), 10);
	CHECK(throwsError([&] { scnseal::findSlot(tail.data(), tail.size()); }));
}

static void testKeysAsText() {
	scnseal::Key key = keyOf(10);
	std::string text = scnseal::formatKey(key);
	CHECK(text.size() == 64);
	CHECK(scnseal::parseKey(text) == key);
	CHECK(scnseal::parseKey("  " + text + "\n") == key);
	std::string upper = text;
	for (auto &c : upper) c = char(toupper(c));
	CHECK(scnseal::parseKey(upper) == key);
	CHECK(throwsError([&] { scnseal::parseKey(text.substr(1)); }));
	CHECK(throwsError([&] { scnseal::parseKey(text + "0"); }));
	CHECK(throwsError([&] { scnseal::parseKey(std::string(63, '0') + "g"); }));
	CHECK(throwsError([&] { scnseal::parseKey(std::string(64, '0')); }));
	scnseal::Key a = scnseal::randomKey(), b = scnseal::randomKey();
	CHECK(a != b);
	scnseal::Key zero{};
	CHECK(a != zero);
	scnseal::wipe(a);
	CHECK(a == zero);
}

int main() {
	testRoundTrip();
	testDeterministic();
	testWrongKeyAndChanges();
	testFormatFields();
	testSlot();
	testKeysAsText();
	std::printf("%d checks, %d failed\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
