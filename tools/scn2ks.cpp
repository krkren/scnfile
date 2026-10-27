// scn2ks - turns a compiled scenario back into a KAG script (.ks).
//
//   scn2ks [options] IN [OUT.ks]
//
// IN is the compiled scenario as JSON: KAGEX's <script>.scn.json (what its compiler writes
// without the scnfile plugin, or with format=json in parseroption.txt), or a .scn dumped as
// JSON. A plain .scn (PSB) holds the same tree and is read too. OUT is IN without .json and
// .scn, ending in .ks (char.ks.scn.json: char.ks).
//
//   --linemode=MODE   the line mode the script starts in: auto (the default: worked out from
//                     the texts), none, page, line, vn, tex or free. A script compiled with
//                     parseroption.txt's linemode= starts in that one
//   --craftername     as parseroption.txt's craftername=true
//   --ignorecr=no     the game's global.ignoreCR is false (line mode none only)
//   --no-bom          no UTF-8 BOM at the start
//   --lang=CODE       in a file of a game in more than one language, write that language's
//                     translation of the script (KAGEX's lang/CODE/scenario/<script>) rather
//                     than the script; OUT defaults to IN's name with .CODE.ks (char.ja.ks)
//   -q                don't list what doesn't compile back exactly
//
// What the script can't say exactly (a text a line mode can't make, a step that isn't a tag)
// is marked in it with "; scn2ks:" comments and listed on stderr; the exit status is 0 all the
// same, 1 for a file it can't read.
//
// SPDX-License-Identifier: 0BSD

#include "psb.hpp"
#include "scnks.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

std::vector<uint8_t> readFile(const std::string &path) {
	std::ifstream in(path, std::ios::binary);
	if (!in) throw std::runtime_error("can't read " + path);
	return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void writeFile(const std::string &path, const std::string &text) {
	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	if (!out) throw std::runtime_error("can't write " + path);
	out.write(text.data(), std::streamsize(text.size()));
	if (!out) throw std::runtime_error("can't write " + path);
}

bool endsWith(const std::string &s, const std::string &end) {
	return s.size() >= end.size() && s.compare(s.size() - end.size(), end.size(), end) == 0;
}

// char.ks.scn.json, char.ks.scn: char.ks; other.json: other.ks. With a language, char.ja.ks
std::string outName(std::string in, const std::string &lang) {
	if (endsWith(in, ".json")) in.resize(in.size() - 5);
	if (endsWith(in, ".scn")) in.resize(in.size() - 4);
	if (endsWith(in, ".ks")) in.resize(in.size() - 3);
	return in + (lang.empty() ? "" : "." + lang) + ".ks";
}

int usage() {
	std::fprintf(stderr,
				 "usage: scn2ks [--linemode=MODE] [--craftername] [--ignorecr=no] [--no-bom] [--lang=CODE] [-q] IN [OUT.ks]\n"
				 "       IN: a compiled scenario's JSON (.scn.json), or a plain .scn\n");
	return 2;
}

} // namespace

int main(int argc, char **argv) {
	scnks::Options opt;
	bool quiet = false;
	std::vector<std::string> files;
	for (int i = 1; i < argc; i++) {
		std::string a = argv[i];
		if (a == "-q" || a == "--quiet") quiet = true;
		else if (a == "--craftername") opt.crAfterName = true;
		else if (a == "--no-bom") opt.bom = false;
		else if (a.rfind("--linemode=", 0) == 0) opt.lineMode = a.substr(11);
		else if (a.rfind("--lang=", 0) == 0) opt.language = a.substr(7);
		else if (a.rfind("--ignorecr=", 0) == 0) opt.ignoreCR = a.substr(11) != "no" && a.substr(11) != "false" && a.substr(11) != "0";
		else if (a == "-h" || a == "--help") return usage();
		else if (!a.empty() && a[0] == '-') {
			std::fprintf(stderr, "unknown option %s\n", a.c_str());
			return usage();
		} else files.push_back(a);
	}
	static const char *modes[] = { "auto", "none", "page", "line", "vn", "tex", "free" };
	bool modeOk = false;
	for (const char *m : modes) modeOk = modeOk || opt.lineMode == m;
	if (!modeOk) {
		std::fprintf(stderr, "unknown line mode %s\n", opt.lineMode.c_str());
		return usage();
	}
	if (files.empty() || files.size() > 2) return usage();
	try {
		std::vector<uint8_t> bytes = readFile(files[0]);
		scnpsb::Value root;
		if (scnpsb::looksLikePsb(bytes.data(), bytes.size())) {
			try {
				root = scnpsb::read(bytes.data(), bytes.size());
			} catch (const scnpsb::Error &e) {
				throw std::runtime_error(files[0] + ": " + e.what());
			}
		} else if (bytes.size() >= 4 && std::memcmp(bytes.data(), "KSCN", 4) == 0) {
			throw std::runtime_error(files[0] + " is encrypted: scn2ks reads a compiled scenario's JSON, or a plain .scn");
		} else {
			try {
				root = scnks::fromJson(std::string(bytes.begin(), bytes.end()));
			} catch (const scnks::Error &e) {
				throw std::runtime_error(files[0] + ": " + e.what());
			}
		}
		std::string name = files[0];
		size_t slash = name.find_last_of("/\\");
		opt.source = slash == std::string::npos ? name : name.substr(slash + 1);
		scnks::Result r = scnks::decompile(root, opt);
		std::string out = files.size() == 2 ? files[1] : outName(files[0], opt.language);
		if (out == files[0]) throw std::runtime_error("won't write over " + files[0]);
		writeFile(out, r.ks);
		if (!quiet) {
			std::string langs;
			for (const auto &l : r.languages) langs += (langs.empty() ? "" : ", ") + l;
			std::fprintf(stderr, "%s: %d scenes, %d texts, line mode %s at the start%s -> %s\n", files[0].c_str(), r.scenes, r.texts,
						 r.startMode.c_str(),
						 langs.empty() ? "" : (" (other languages: " + langs + (opt.language.empty() ? "" : "; written in " + opt.language) + ")").c_str(),
						 out.c_str());
			if (!r.notes.empty()) {
				std::fprintf(stderr, "%zu things don't compile back exactly (marked \"; scn2ks:\" in the script):\n", r.notes.size());
				for (const auto &n : r.notes) std::fprintf(stderr, "  %s\n", n.c_str());
			}
		}
	} catch (const std::exception &e) {
		std::fprintf(stderr, "%s\n", e.what());
		return 1;
	}
	return 0;
}
