// scnseal - encrypts compiled scenarios and writes a game's key into the scnfile plugin.
// kagex-release does the same with the same code (plugins/scnfile/seal.cpp); this is the tool
// for doing it by hand or from a script.
//
//   scnseal keygen KEYFILE                   makes a new random key
//   scnseal seal -k KEYFILE FILE...          encrypts plain .scn files where they are
//   scnseal open -k KEYFILE IN OUT           decrypts one, to look inside
//   scnseal setkey -k KEYFILE BINARY [OUT]   writes the key into a plugin or an engine with
//                                            the plugin built in (into OUT, or in place)
//   scnseal check BINARY                     says whether a binary's key slot holds a key
//
// A key file holds the key as 64 hexadecimal digits. Keep it out of the game's repository.
//
// SPDX-License-Identifier: 0BSD

#include "psb.hpp"
#include "seal.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

std::vector<uint8_t> readFile(const std::string &path) {
	std::ifstream in(path, std::ios::binary);
	if (!in) throw scnseal::Error("can't read " + path);
	return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// Writes next to the file first, then replaces it, so a failure leaves the old one
void writeFile(const std::string &path, const std::vector<uint8_t> &bytes) {
	std::string tmp = path + ".scnseal-tmp";
	{
		std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
		if (!out) throw scnseal::Error("can't write " + tmp);
		out.write(reinterpret_cast<const char *>(bytes.data()), std::streamsize(bytes.size()));
		if (!out) throw scnseal::Error("can't write " + tmp);
	}
	if (std::rename(tmp.c_str(), path.c_str()) != 0) {
		std::remove(path.c_str());
		if (std::rename(tmp.c_str(), path.c_str()) != 0) throw scnseal::Error("can't replace " + path);
	}
}

scnseal::Key readKey(const std::string &path) {
	std::vector<uint8_t> bytes = readFile(path);
	std::string text(bytes.begin(), bytes.end());
	scnseal::Key key = scnseal::parseKey(text);
	std::fill(text.begin(), text.end(), '\0');
	return key;
}

int usage() {
	std::fprintf(stderr,
				 "usage: scnseal keygen KEYFILE\n"
				 "       scnseal seal -k KEYFILE FILE...\n"
				 "       scnseal open -k KEYFILE IN OUT\n"
				 "       scnseal setkey -k KEYFILE BINARY [OUT]\n"
				 "       scnseal check BINARY\n");
	return 2;
}

} // namespace

int main(int argc, char **argv) {
	std::vector<std::string> args(argv + 1, argv + argc);
	if (args.empty()) return usage();
	std::string cmd = args[0];
	args.erase(args.begin());
	std::string keyFile;
	if (args.size() >= 2 && (args[0] == "-k" || args[0] == "--key")) {
		keyFile = args[1];
		args.erase(args.begin(), args.begin() + 2);
	}
	try {
		if (cmd == "keygen") {
			if (args.size() != 1) return usage();
			std::ifstream exists(args[0]);
			if (exists) throw scnseal::Error(args[0] + " already exists: a key isn't replaced by mistake");
			scnseal::Key key = scnseal::randomKey();
			std::string text = scnseal::formatKey(key) + "\n";
			writeFile(args[0], std::vector<uint8_t>(text.begin(), text.end()));
			scnseal::wipe(key);
			std::printf("wrote a new key to %s\n", args[0].c_str());
		} else if (cmd == "seal") {
			if (keyFile.empty() || args.empty()) return usage();
			scnseal::Key key = readKey(keyFile);
			for (const std::string &path : args) {
				std::vector<uint8_t> psb = readFile(path);
				if (scnseal::isSealed(psb.data(), psb.size())) throw scnseal::Error(path + " is already encrypted");
				scnpsb::read(psb.data(), psb.size()); // a PSB this plugin reads
				std::vector<uint8_t> sealed = scnseal::seal(psb, key);
				writeFile(path, sealed);
				std::printf("%s: %zu -> %zu bytes\n", path.c_str(), psb.size(), sealed.size());
			}
			scnseal::wipe(key);
		} else if (cmd == "open") {
			if (keyFile.empty() || args.size() != 2) return usage();
			scnseal::Key key = readKey(keyFile);
			std::vector<uint8_t> sealed = readFile(args[0]);
			std::vector<uint8_t> psb = scnseal::unseal(sealed.data(), sealed.size(), key);
			scnseal::wipe(key);
			writeFile(args[1], psb);
		} else if (cmd == "setkey") {
			if (keyFile.empty() || args.empty() || args.size() > 2) return usage();
			scnseal::Key key = readKey(keyFile);
			std::vector<uint8_t> binary = readFile(args[0]);
			scnseal::writeSlot(binary, key);
			scnseal::wipe(key);
			writeFile(args.size() == 2 ? args[1] : args[0], binary);
			std::printf("wrote the key into %s\n", (args.size() == 2 ? args[1] : args[0]).c_str());
		} else if (cmd == "check") {
			if (args.size() != 1) return usage();
			std::vector<uint8_t> binary = readFile(args[0]);
			scnseal::Key key;
			bool has = scnseal::readSlot(binary.data(), binary.size(), key);
			scnseal::wipe(key);
			std::printf("%s: %s\n", args[0].c_str(), has ? "holds a key" : "has an empty key slot");
		} else {
			return usage();
		}
	} catch (const std::exception &e) {
		std::fprintf(stderr, "%s\n", e.what());
		return 1;
	}
	return 0;
}
