// seal.cpp - scnfile's encrypted files and the game key's slot. See seal.hpp.
//
// SPDX-License-Identifier: 0BSD

#include "seal.hpp"

#include "monocypher/monocypher.h"

#include <cstring>

#ifdef SCNFILE_HAVE_ZLIB
#include <zlib.h>
#endif

namespace scnseal {

namespace {

const uint8_t MAGIC[4] = { 'K', 'S', 'C', 'N' };
const uint8_t VERSION = 1;
const uint8_t FLAG_ZLIB = 1;

// "KSCN-KEY-SLOT-v1", each byte inverted
const uint8_t MARKER_INVERTED[SLOT_MARKER_SIZE] = {
	0xB4, 0xAC, 0xBC, 0xB1, 0xD2, 0xB4, 0xBA, 0xA6, 0xD2, 0xAC, 0xB3, 0xB0, 0xAB, 0xD2, 0x89, 0xCE,
};

void putU32(uint8_t *p, uint32_t v) {
	for (int k = 0; k < 4; k++) p[k] = uint8_t(v >> (8 * k));
}

uint32_t getU32(const uint8_t *p) {
	return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

// The keys a game key gives: one to encrypt with, one to make nonces with
struct Subkeys {
	uint8_t encrypt[32];
	uint8_t nonce[32];

	explicit Subkeys(const Key &key) {
		static const char ENC[] = "scnfile encrypt v1";
		static const char NON[] = "scnfile nonce v1";
		crypto_blake2b_keyed(encrypt, 32, key.data(), 32, reinterpret_cast<const uint8_t *>(ENC), sizeof(ENC) - 1);
		crypto_blake2b_keyed(nonce, 32, key.data(), 32, reinterpret_cast<const uint8_t *>(NON), sizeof(NON) - 1);
	}
	~Subkeys() {
		crypto_wipe(encrypt, sizeof(encrypt));
		crypto_wipe(nonce, sizeof(nonce));
	}
};

int hexDigit(char c) {
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

} // namespace

bool isSealed(const uint8_t *data, size_t size) {
	return size >= 4 && std::memcmp(data, MAGIC, 4) == 0;
}

std::vector<uint8_t> seal(const std::vector<uint8_t> &psb, const Key &key, bool compress) {
	if (psb.size() > 0x7FFFFFFFu) throw Error("scnseal: the file is too large");
	std::vector<uint8_t> content;
	uint8_t flags = 0;
#ifdef SCNFILE_HAVE_ZLIB
	if (compress) {
		uLongf len = compressBound(uLong(psb.size()));
		content.resize(len);
		if (compress2(content.data(), &len, psb.data(), uLong(psb.size()), 9) != Z_OK) {
			throw Error("scnseal: zlib couldn't compress the file");
		}
		content.resize(len);
		flags |= FLAG_ZLIB;
	}
#else
	(void)compress;
#endif
	if (!(flags & FLAG_ZLIB)) content = psb;

	Subkeys sub(key);
	std::vector<uint8_t> out(HEADER_SIZE + content.size() + TAG_SIZE, 0);
	std::memcpy(out.data(), MAGIC, 4);
	out[4] = VERSION;
	out[5] = flags;
	putU32(&out[8], uint32_t(psb.size()));
	crypto_blake2b_keyed(&out[16], 24, sub.nonce, 32, psb.data(), psb.size());
	crypto_aead_lock(&out[HEADER_SIZE], &out[HEADER_SIZE + content.size()], sub.encrypt, &out[16],
					 out.data(), HEADER_SIZE, content.data(), content.size());
	crypto_wipe(content.data(), content.size());
	return out;
}

std::vector<uint8_t> unseal(const uint8_t *data, size_t size, const Key &key) {
	if (!isSealed(data, size)) throw Error("scnseal: not an encrypted scnfile file");
	if (size < HEADER_SIZE + TAG_SIZE) throw Error("scnseal: the encrypted file is cut short");
	if (data[4] != VERSION) throw Error("scnseal: encrypted file version " + std::to_string(data[4]) + " isn't supported");
	uint8_t flags = data[5];
	if (flags & ~FLAG_ZLIB) throw Error("scnseal: the encrypted file has flags this build doesn't know");
	uint32_t psbSize = getU32(data + 8);
	if (psbSize > 0x7FFFFFFFu) throw Error("scnseal: the encrypted file is corrupt");

	size_t n = size - HEADER_SIZE - TAG_SIZE;
	std::vector<uint8_t> content(n ? n : 1);
	Subkeys sub(key);
	if (crypto_aead_unlock(content.data(), data + HEADER_SIZE + n, sub.encrypt, data + 16, data, HEADER_SIZE,
						   data + HEADER_SIZE, n) != 0) {
		throw Error("scnseal: the encrypted file doesn't open with this key (or has been changed)");
	}
	content.resize(n);
	if (!(flags & FLAG_ZLIB)) {
		if (content.size() != psbSize) throw Error("scnseal: the encrypted file is corrupt");
		return content;
	}
#ifdef SCNFILE_HAVE_ZLIB
	std::vector<uint8_t> psb(psbSize ? psbSize : 1);
	uLongf len = psbSize;
	int r = uncompress(psb.data(), &len, content.data(), uLong(content.size()));
	crypto_wipe(content.data(), content.size());
	if (r != Z_OK || len != psbSize) throw Error("scnseal: the encrypted file's zlib data is corrupt");
	psb.resize(psbSize);
	return psb;
#else
	throw Error("scnseal: this build can't expand compressed (zlib) files");
#endif
}

std::array<uint8_t, SLOT_MARKER_SIZE> slotMarker() {
	std::array<uint8_t, SLOT_MARKER_SIZE> m;
	for (size_t k = 0; k < SLOT_MARKER_SIZE; k++) m[k] = uint8_t(~MARKER_INVERTED[k]);
	return m;
}

size_t findSlot(const uint8_t *data, size_t size) {
	std::array<uint8_t, SLOT_MARKER_SIZE> m = slotMarker();
	size_t found = 0, at = 0;
	if (size >= SLOT_SIZE) {
		for (size_t k = 0; k + SLOT_SIZE <= size; k++) {
			if (data[k] == m[0] && std::memcmp(data + k, m.data(), SLOT_MARKER_SIZE) == 0) {
				found++;
				at = k;
			}
		}
	}
	if (found == 0) throw Error("scnseal: no key slot in this file (is it the scnfile plugin, or an engine with it built in?)");
	if (found > 1) throw Error("scnseal: more than one key slot in this file");
	return at;
}

void writeSlot(std::vector<uint8_t> &binary, const Key &key) {
	size_t at = findSlot(binary.data(), binary.size());
	std::memcpy(&binary[at + SLOT_MARKER_SIZE], key.data(), 32);
}

bool readSlot(const uint8_t *data, size_t size, Key &key) {
	size_t at = findSlot(data, size);
	bool any = false;
	for (size_t k = 0; k < 32; k++) {
		key[k] = data[at + SLOT_MARKER_SIZE + k];
		any = any || key[k] != 0;
	}
	return any;
}

Key parseKey(const std::string &text) {
	size_t a = 0, b = text.size();
	while (a < b && (text[a] == ' ' || text[a] == '\t' || text[a] == '\r' || text[a] == '\n')) a++;
	while (b > a && (text[b - 1] == ' ' || text[b - 1] == '\t' || text[b - 1] == '\r' || text[b - 1] == '\n')) b--;
	if (b - a != 64) throw Error("scnseal: a key is 64 hexadecimal digits");
	Key key;
	bool any = false;
	for (size_t k = 0; k < 32; k++) {
		int hi = hexDigit(text[a + 2 * k]), lo = hexDigit(text[a + 2 * k + 1]);
		if (hi < 0 || lo < 0) throw Error("scnseal: a key is 64 hexadecimal digits");
		key[k] = uint8_t(hi * 16 + lo);
		any = any || key[k] != 0;
	}
	if (!any) throw Error("scnseal: a key of all zeros means no key");
	return key;
}

std::string formatKey(const Key &key) {
	static const char *digits = "0123456789abcdef";
	std::string s;
	for (uint8_t b : key) {
		s.push_back(digits[b >> 4]);
		s.push_back(digits[b & 15]);
	}
	return s;
}

void wipe(Key &key) {
	crypto_wipe(key.data(), key.size());
}

} // namespace scnseal
