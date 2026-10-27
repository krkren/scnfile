// scnks.hpp - turns a compiled scenario (the tree of a .scn.json or .scn file) back into a
// KAG script.
//
// A compiled scenario is what KAGEX's compiler (template/system/SceneParser.tjs in
// krkren/kagex, described in its doc/scn.txt) makes of a .ks script, and what Wamsoft's
// tools made for their games: per scene, its texts and the steps between them. This writes a
// script that compiles back to the same scenes: the texts as KAG writes them in the line mode
// in force (【name】, [l], [p], [r], ruby), the steps as tags, [if] blocks, [iscript] blocks,
// jumps and choices.
//
// A game in more than one language has every language's texts, display names, choices and
// titles in its compiled files (Wamsoft's layout; KAGEX's doc/scn.txt, "Languages"): the script
// is written in the one Options::language names, and comes out as that language's translation
// of the script (what KAGEX compiles from lang/<code>/scenario/<script>).
//
// The compiler's own text rules are modelled here (the line modes, names, voices, the waits
// it adds at line ends), and each scene is checked against that model as it's written: what
// can't be written so that it compiles back the same is reported (Result::notes) and marked in
// the script with a "; scn2ks:" comment, and the script says it as near as it can.
//
// SPDX-License-Identifier: 0BSD

#pragma once

#include "psb.hpp"

#include <stdexcept>
#include <string>
#include <vector>

namespace scnks {

struct Options {
	// The line mode the script starts in: auto (worked out from its texts), none, page, line,
	// vn, tex or free. parseroption.txt's linemode, for scripts compiled with one
	std::string lineMode = "auto";
	// parseroption.txt's craftername
	bool crAfterName = false;
	// The game's global.ignoreCR (KAGEX's default is true), for line mode none
	bool ignoreCR = true;
	// Start the script with a UTF-8 BOM (KAGEX's scripts have one)
	bool bom = true;
	// The file's name, for the comment at the top
	std::string source;
	// In a file with more than one language, the one to write: one of its languages' codes
	// (Result::languages), or empty for the one the script is written in
	std::string language;
};

struct Result {
	std::string ks;                 // the script (UTF-8)
	std::vector<std::string> notes; // what doesn't compile back exactly, by scene
	std::string startMode;          // the line mode it was written for at the start
	int scenes = 0;
	int texts = 0;
	std::vector<std::string> languages; // the file's languages after its own (none: one)
};

// Raised for a tree that isn't a compiled scenario
class Error : public std::runtime_error {
public:
	using std::runtime_error::runtime_error;
};

Result decompile(const scnpsb::Value &root, const Options &options = Options());

// The tree of a compiled scenario's JSON: KAGEX's <script>.scn.json (what its compiler writes
// without the scnfile plugin, or with format=json in parseroption.txt), or a .scn dumped as
// JSON. UTF-8 (a BOM is skipped) or UTF-16 with a BOM. Raises Error, saying where, for text
// that isn't JSON
scnpsb::Value fromJson(const std::string &text);

// The tree as JSON (for looking inside a .scn). Resources are shown by their size
std::string toJson(const scnpsb::Value &value);

} // namespace scnks
