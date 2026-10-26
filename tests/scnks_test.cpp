// scnks_test.cpp - tests of tools/scnks.cpp without an engine: the JSON reader, and scripts
// written from compiled scenarios made by hand (tests/run_scn2ks_test.py compiles real ones
// and back in KAGEX).
//
// SPDX-License-Identifier: 0BSD

#include "scnks.hpp"

#include <cstdio>
#include <string>

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

using scnpsb::Value;

static bool refused(const std::string &json) {
	try {
		scnks::fromJson(json);
	} catch (const scnks::Error &) {
		return true;
	}
	return false;
}

static void testJson() {
	Value v = scnks::fromJson(" {\"a\": [1, -2, 3.5, 1e3, \"x\\n\\\"\\u00e9\\ud83d\\ude00\", null, true, false, {}, []], \"b\": {\"c\": \"\"}} ");
	CHECK(v.type == Value::Type::Object && v.object->size() == 2);
	const Value &a = (*v.object)[0].second;
	CHECK((*v.object)[0].first == "a" && a.list->size() == 10);
	CHECK((*a.list)[0].type == Value::Type::Int && (*a.list)[0].i == 1);
	CHECK((*a.list)[1].i == -2);
	CHECK((*a.list)[2].type == Value::Type::Real && (*a.list)[2].d == 3.5);
	CHECK((*a.list)[3].type == Value::Type::Real && (*a.list)[3].d == 1000);
	CHECK((*a.list)[4].s == "x\n\"\xC3\xA9\xF0\x9F\x98\x80");
	CHECK((*a.list)[5].type == Value::Type::Null);
	CHECK((*a.list)[6].type == Value::Type::Bool && (*a.list)[6].b);
	CHECK((*a.list)[8].type == Value::Type::Object && (*a.list)[9].type == Value::Type::List);
	// a BOM, UTF-16 with its BOM
	CHECK(scnks::fromJson("\xEF\xBB\xBF[1]").list->size() == 1);
	std::string u16 = std::string("\xFF\xFE", 2) + std::string("[\0\"\0\x42\x30\"\0]\0", 10);
	Value w = scnks::fromJson(u16);
	CHECK(w.type == Value::Type::List && (*w.list)[0].s == "\xE3\x81\x82");
	// what isn't JSON
	CHECK(refused(""));
	CHECK(refused("[1, 2"));
	CHECK(refused("{\"a\" 1}"));
	CHECK(refused("[1] x"));
	CHECK(refused("\"\\q\""));
	CHECK(refused("[tru]"));
	// the same tree back
	std::string j = scnks::toJson(v);
	CHECK(scnpsb::equal(scnks::fromJson(j), v));
}

// A scene as KAGEX's compiler writes it, in page mode
static const char *pageScene = R"json({
 "version": 1, "storage": "hand.ks", "hash": "x",
 "scenes": [{
  "label": "*start", "title": "Start", "firstLine": 0, "startLine": 0, "lastLine": 9, "spCount": 2,
  "texts": [
   ["ai", null, "Hello.", [{"name": "ai", "voice": 3}], 0, {}],
   [null, null, "A line\nwith [るび]字 and 50\\%.", null, 0, 1]
  ],
  "lines": [
   [0, {}, 1, null, 0, 0, null], ["linemode", 0, "mode", "page"], ["ai", 0, "voice", "3"], 1,
   [1, 1, 2, null, 3, 0, null], 2, [2, 2, null, null, 4, 0, null]
  ],
  "nexts": [{"type": 1, "text": "Go", "target": "*start", "eval": "(f.a)"}, {"type": 1, "text": "Stay", "target": "*start"}]
 }]
})json";

static void testPage() {
	scnks::Options o;
	o.source = "hand.json";
	scnks::Result r = scnks::decompile(scnks::fromJson(pageScene), o);
	const std::string want =
		"\xEF\xBB\xBF; Written by scn2ks from hand.json (compiled from hand.ks)\n"
		"; Line mode at the start: none\n"
		"*start|Start\n"
		"[linemode mode=page]\n"
		"[ai voice=3]\n"
		"【ai】Hello.\n"
		"A line[r]with [ruby text=るび]字 and 50%.\n"
		"[seladd text=Go target=*start eval=f.a]\n"
		"[seladd text=Stay target=*start]\n"
		"[select]\n";
	CHECK(r.ks == want);
	if (r.ks != want) std::printf("--- got:\n%s--- want:\n%s", r.ks.c_str(), want.c_str());
	CHECK(r.notes.empty());
	CHECK(r.scenes == 1 && r.texts == 2);
}

// Line mode vn: a line break is [l] (the next line carries on the message), an empty line [p]
static const char *vnScene = R"json({
 "scenes": [{
  "label": "*a", "title": "",
  "texts": [
   ["ai", null, "One.", [{"name": "ai", "voice": 7}], 1, {}],
   ["ai", null, "\nTwo.", null, 64, 1],
   [null, null, "Three.", null, 0, 1]
  ],
  "lines": [["linemode", 0, "mode", "vn"], 1, 2, 3]
 }]
})json";

static void testVn() {
	scnks::Result r = scnks::decompile(scnks::fromJson(vnScene));
	CHECK(r.notes.empty());
	// the voice comes from a counter set before the line ([voice] isn't kept as a step)
	CHECK(r.ks.find("[voice ai=7]【ai】One.\nTwo.\n\nThree.\n") != std::string::npos);
	if (!r.notes.empty() || r.ks.find("Two.\n\nThree.") == std::string::npos) std::printf("%s", r.ks.c_str());
}

// What a line mode can't say is reported and marked
static void testNotes() {
	const char *json = R"json({"scenes": [{"label": "*a", "title": "",
		"texts": [["ai", null, "Ends with [l].", null, 1, {}]],
		"lines": [["linemode", 0, "mode", "page"], 1]}]})json";
	scnks::Result r = scnks::decompile(scnks::fromJson(json));
	CHECK(!r.notes.empty());
	CHECK(r.ks.find("; scn2ks:") != std::string::npos);

	// a step that isn't a tag is kept as a comment
	const char *odd = R"json({"scenes": [{"label": "", "title": "", "texts": [],
		"lines": [["call", 0, "storage", "x.ks"]]}]})json";
	scnks::Result c = scnks::decompile(scnks::fromJson(odd));
	CHECK(!c.notes.empty());
	CHECK(c.ks.find("; scn2ks: [call storage=x.ks]") != std::string::npos);

	// not a compiled scenario
	bool threw = false;
	try {
		scnks::decompile(scnks::fromJson("{\"a\": 1}"));
	} catch (const scnks::Error &) {
		threw = true;
	}
	CHECK(threw);
}

int main() {
	testJson();
	testPage();
	testVn();
	testNotes();
	std::printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
