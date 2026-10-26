// seal.hpp - scnfile's encrypted files and the game key's slot, with no engine dependencies
// (the plugin decrypts; tools/scnseal.cpp and kagex-release encrypt and write keys).
//
// An encrypted file ("sealed", docs/format.md):
//
//   0   4   "KSCN"
//   4   1   format version (1)
//   5   1   flags: 1 = the content is zlib-compressed
//   6   2   0
//   8   4   the plain PSB's size
//   12  4   0
//   16  24  nonce
//   40  n   the content (the PSB, compressed or not), encrypted with XChaCha20
//   40+n 16 the Poly1305 tag over the header (bytes 0-39) and the encrypted content
//
// The game key (32 bytes) isn't used directly: an encryption key and a nonce key are derived
// from it with keyed BLAKE2b, and the nonce is the nonce key's BLAKE2b of the PSB, so the same
// PSB and key always give the same file (builds can be compared) and different PSBs never
// share a nonce.
//
// SPDX-License-Identifier: 0BSD

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace scnseal {

class Error : public std::runtime_error {
public:
	using std::runtime_error::runtime_error;
};

using Key = std::array<uint8_t, 32>;

const size_t HEADER_SIZE = 40;
const size_t TAG_SIZE = 16;

// Whether the bytes start like an encrypted file
bool isSealed(const uint8_t *data, size_t size);

// An encrypted file holding a plain PSB. compress: zlib the PSB first (when built with zlib)
std::vector<uint8_t> seal(const std::vector<uint8_t> &psb, const Key &key, bool compress = true);

// The plain PSB in an encrypted file. Raises Error when the key is wrong or the file isn't
// exactly as it was encrypted
std::vector<uint8_t> unseal(const uint8_t *data, size_t size, const Key &key);

// The key slot: the plugin holds SLOT_MARKER_SIZE bytes of marker followed by the key's 32
// bytes, all 0 until a game's key is written there
const size_t SLOT_MARKER_SIZE = 16;
const size_t SLOT_SIZE = SLOT_MARKER_SIZE + 32;

// The marker (kept here inverted, so code that searches for it doesn't hold a second copy)
std::array<uint8_t, SLOT_MARKER_SIZE> slotMarker();

// Where the slot is in a binary (a plugin, or an engine the plugin is linked into). Raises
// Error unless there's exactly one
size_t findSlot(const uint8_t *data, size_t size);

// Writes a key into the slot of a binary
void writeSlot(std::vector<uint8_t> &binary, const Key &key);

// The key in a binary's slot; false when none has been written
bool readSlot(const uint8_t *data, size_t size, Key &key);

// Keys as text: 64 hexadecimal digits (whitespace around them ignored)
Key parseKey(const std::string &text);
std::string formatKey(const Key &key);

// Clears a key from memory
void wipe(Key &key);

// A new key from the system's random source (seal_random.cpp, which the plugin leaves out)
Key randomKey();

} // namespace scnseal
