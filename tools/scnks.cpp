// scnks.cpp - turns a compiled scenario back into a KAG script (scnks.hpp).
//
// The compiled format is krkren/kagex's doc/scn.txt. Writing a script that compiles back to
// the same scenes takes knowing what the compiler (krkren/kagex
// template/system/SceneParser.tjs) makes of each thing written: the line modes put messages
// together from lines and add waits at their ends, a name starts a message, voices come from
// counters that tags set, [if] branches each start from the state at [if]. The Model below
// follows those rules for what this file writes, and the Emitter tries, for each text and
// step of a scene, the ways of writing it in turn, keeping the first that the Model says
// compiles to what the file holds.
//
// SPDX-License-Identifier: 0BSD

#include "scnks.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <optional>
#include <utility>

namespace scnks {
namespace {

using scnpsb::Value;
using VT = Value::Type;
using OStr = std::optional<std::string>;

// ------------------------------------------------------------------------
// The tree

const Value *member(const Value *obj, const char *name) {
	if (obj == nullptr || obj->type != VT::Object) return nullptr;
	for (const auto &m : *obj->object) {
		if (m.first == name) return &m.second;
	}
	return nullptr;
}

bool isNull(const Value *v) { return v == nullptr || v->type == VT::Null; }
bool isString(const Value *v) { return v != nullptr && v->type == VT::String; }
bool isInt(const Value *v) { return v != nullptr && (v->type == VT::Int || v->type == VT::Bool); }
bool isList(const Value *v) { return v != nullptr && v->type == VT::List; }

int64_t intOf(const Value *v) {
	if (v == nullptr) return 0;
	if (v->type == VT::Int) return v->i;
	if (v->type == VT::Bool) return v->b ? 1 : 0;
	if (v->type == VT::Real) return int64_t(v->d);
	return 0;
}

std::string numberText(double d) {
	char buf[40];
	std::snprintf(buf, sizeof buf, "%.17g", d);
	// the shortest that reads back the same
	for (int p = 1; p <= 17; p++) {
		char t[40];
		std::snprintf(t, sizeof t, "%.*g", p, d);
		if (std::strtod(t, nullptr) == d) return t;
	}
	return buf;
}

// A scalar as the text a tag attribute holds
std::string scalarText(const Value &v) {
	switch (v.type) {
	case VT::String: return v.s;
	case VT::Int: return std::to_string(v.i);
	case VT::Bool: return v.b ? "1" : "0";
	case VT::Real: return numberText(v.d);
	default: return "";
	}
}

Value strValue(const std::string &s) { return Value::makeString(s); }
Value intValue(int64_t i) { return Value::makeInt(i); }

Value makeStep(std::initializer_list<Value> items) {
	Value v = Value::makeList();
	for (const Value &i : items) v.list->push_back(i);
	return v;
}

// ------------------------------------------------------------------------
// JSON

void jsonString(std::string &out, const std::string &s) {
	out += '"';
	for (unsigned char c : s) {
		switch (c) {
		case '"': out += "\\\""; break;
		case '\\': out += "\\\\"; break;
		case '\n': out += "\\n"; break;
		case '\r': out += "\\r"; break;
		case '\t': out += "\\t"; break;
		default:
			if (c < 0x20) {
				char buf[8];
				std::snprintf(buf, sizeof buf, "\\u%04x", c);
				out += buf;
			} else {
				out += char(c);
			}
		}
	}
	out += '"';
}

bool isScalar(const Value &v) {
	return v.type != VT::List && v.type != VT::Object;
}

void json(std::string &out, const Value &v, int depth) {
	std::string pad(size_t(depth) * 2, ' ');
	switch (v.type) {
	case VT::Null: out += "null"; return;
	case VT::Bool: out += v.b ? "true" : "false"; return;
	case VT::Int: out += std::to_string(v.i); return;
	case VT::Real: out += numberText(v.d); return;
	case VT::String: jsonString(out, v.s); return;
	case VT::Resource: jsonString(out, "<resource: " + std::to_string(v.s.size()) + " bytes>"); return;
	case VT::List: {
		if (v.list->empty()) { out += "[]"; return; }
		bool flat = true;
		for (const Value &i : *v.list) flat = flat && isScalar(i);
		if (flat) {
			out += '[';
			for (size_t i = 0; i < v.list->size(); i++) {
				if (i) out += ", ";
				json(out, (*v.list)[i], depth + 1);
			}
			out += ']';
			return;
		}
		out += "[\n";
		for (size_t i = 0; i < v.list->size(); i++) {
			out += pad + "  ";
			json(out, (*v.list)[i], depth + 1);
			out += i + 1 < v.list->size() ? ",\n" : "\n";
		}
		out += pad + "]";
		return;
	}
	case VT::Object: {
		if (v.object->empty()) { out += "{}"; return; }
		out += "{\n";
		for (size_t i = 0; i < v.object->size(); i++) {
			out += pad + "  ";
			jsonString(out, (*v.object)[i].first);
			out += ": ";
			json(out, (*v.object)[i].second, depth + 1);
			out += i + 1 < v.object->size() ? ",\n" : "\n";
		}
		out += pad + "}";
		return;
	}
	}
}

std::string shortJson(const Value &v) {
	std::string s;
	json(s, v, 0);
	std::string r;
	bool space = false;
	for (char c : s) {
		if (c == '\n') { space = true; continue; }
		if (space && c == ' ') continue;
		space = false;
		r += c;
	}
	if (r.size() > 200) r = r.substr(0, 197) + "...";
	return r;
}

// ------------------------------------------------------------------------
// UTF-8

// The characters of a string (each as its bytes)
std::vector<std::string> utf8Chars(const std::string &s) {
	std::vector<std::string> r;
	for (size_t i = 0; i < s.size();) {
		unsigned char c = s[i];
		size_t n = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
		if (i + n > s.size()) n = s.size() - i;
		r.push_back(s.substr(i, n));
		i += n;
	}
	return r;
}

bool hasAsciiUpper(const std::string &s) {
	for (char c : s) {
		if (c >= 'A' && c <= 'Z') return true;
	}
	return false;
}

// ------------------------------------------------------------------------
// Values as the voice code keeps them (TJS: void, an integer or a string)

struct VVal {
	enum Kind { Void, Int, Str } kind = Void;
	int64_t i = 0;
	std::string s;

	static VVal integer(int64_t v) { VVal r; r.kind = Int; r.i = v; return r; }
	static VVal string(const std::string &v) { VVal r; r.kind = Str; r.s = v; return r; }
	bool isVoid() const { return kind == Void; }
	bool operator==(const VVal &o) const {
		if (kind != o.kind) return false;
		return kind == Void || (kind == Int ? i == o.i : s == o.s);
	}
	bool operator!=(const VVal &o) const { return !(*this == o); }
	std::string text() const { return kind == Int ? std::to_string(i) : kind == Str ? s : "void"; }
};

VVal vvalOf(const Value *v) {
	if (isNull(v)) return VVal();
	if (v->type == VT::String) return VVal::string(v->s);
	if (v->type == VT::Real) return VVal::integer(int64_t(v->d));
	return VVal::integer(intOf(v));
}

// TJS's number of a string ("abc" is 0)
double tjsNumber(const std::string &s) {
	if (s == "true") return 1;
	return std::strtod(s.c_str(), nullptr);
}

double numOf(const VVal &v) {
	return v.kind == VVal::Int ? double(v.i) : v.kind == VVal::Str ? tjsNumber(v.s) : 0;
}

// TJS's truth of a value (checked in the engine: "true" and "1" are true, "abc" and "0" false)
bool truthy(const VVal &v) {
	if (v.kind == VVal::Void) return false;
	if (v.kind == VVal::Int) return v.i != 0;
	return tjsNumber(v.s) != 0;
}

bool digitsOnly(const std::string &s) {
	if (s == "0") return true;
	if (s.empty() || s[0] < '1' || s[0] > '9') return false;
	for (char c : s) {
		if (c < '0' || c > '9') return false;
	}
	return true;
}

// One voice of a text: %[name, voice, noplayvoice]
struct VEntry {
	std::string name;
	VVal voice;
	VVal noplayvoice;
	bool operator==(const VEntry &o) const { return name == o.name && voice == o.voice && noplayvoice == o.noplayvoice; }
};

std::string voicesText(const std::vector<VEntry> &list) {
	if (list.empty()) return "none";
	std::string r;
	for (const VEntry &e : list) {
		if (!r.empty()) r += ", ";
		r += e.name + "=" + e.voice.text();
		if (!e.noplayvoice.isVoid()) r += " (noplayvoice " + e.noplayvoice.text() + ")";
	}
	return r;
}

// ------------------------------------------------------------------------
// What the compiler produces: texts and steps (save points and read flags aren't compared:
// they come from where the waits are and from the World)

const int F_LINE = 0x01, F_NOER = 0x02, F_NOWAIT = 0x04, F_NOTEXT = 0x20, F_CONT = 0x40;
const int F_COMPARED = F_LINE | F_NOER | F_NOWAIT | F_CONT;

struct Event {
	bool isText = false;
	// a text
	OStr name, disp;
	std::string text;
	std::vector<VEntry> voice; // empty: none
	bool voiceUnknown = false;
	int flag = 0;
	int id = 0; // its number in the scene (1-based) for a target
	// a step
	Value step;
};

std::string flagText(int flag) {
	std::string r;
	if (flag & F_LINE) r += "[l]";
	else if (flag & F_NOWAIT) r += "no wait";
	else r += "[p]";
	if (flag & F_NOER) r += " noer";
	if (flag & F_CONT) r += " carried on";
	return r;
}

// Whether two steps are the same, but for what can't be written back the same: the macro
// name in *mp (the script names its own) and where an [iscript] block was
bool sameStep(const Value &a, const Value &b) {
	if (a.type != VT::List || b.type != VT::List) return scnpsb::equal(a, b);
	if (a.list->size() != b.list->size()) return false;
	bool script = !a.list->empty() && (*a.list)[0].type == VT::String && (*a.list)[0].s == "*script";
	for (size_t i = 0; i < a.list->size(); i++) {
		const Value &x = (*a.list)[i], &y = (*b.list)[i];
		if (script && i >= 4) break;
		if (i > 0 && x.type == VT::Object && y.type == VT::Object && (*a.list)[i - 1].type == VT::String &&
			(*a.list)[i - 1].s == "*mp") {
			Value xa = Value::makeObject(), yb = Value::makeObject();
			for (const auto &m : *x.object) if (m.first != "tagname") xa.object->push_back(m);
			for (const auto &m : *y.object) if (m.first != "tagname") yb.object->push_back(m);
			if (!scnpsb::equal(xa, yb)) return false;
			continue;
		}
		if (!scnpsb::equal(x, y)) return false;
	}
	return true;
}

// Whether what came out is what the file holds; why not in `why`
bool sameEvent(const Event &got, const Event &want, std::string *why) {
	if (got.isText != want.isText) {
		if (why) *why = got.isText ? "a text comes out where the file has a step" : "a step comes out where the file has a text";
		return false;
	}
	if (!got.isText) {
		if (sameStep(got.step, want.step)) return true;
		if (why) *why = "the step " + shortJson(want.step) + " comes out as " + shortJson(got.step);
		return false;
	}
	std::string w;
	if (got.name != want.name) w = "its name";
	else if (got.disp != want.disp) w = "its display name";
	else if (got.text != want.text) w = "its text";
	else if ((got.flag & F_COMPARED) != (want.flag & F_COMPARED)) w = "how it ends (" + flagText(got.flag) + ", not " + flagText(want.flag) + ")";
	else if (got.voiceUnknown || !(got.voice == want.voice)) w = "its voice (" + (got.voiceUnknown ? std::string("unknown") : voicesText(got.voice)) + ", not " + voicesText(want.voice) + ")";
	if (w.empty()) return true;
	if (why) *why = "text " + std::to_string(want.id) + " differs in " + w;
	return false;
}

// ------------------------------------------------------------------------
// The compiler's text rules (SceneParser.tjs: the line modes, names, voices, [if])

enum Mode { M_NONE, M_PAGE, M_LINE, M_VN, M_TEX, M_FREE };

int modeOf(const std::string &m) {
	if (m == "page") return M_PAGE;
	if (m == "line") return M_LINE;
	if (m == "vn") return M_VN;
	if (m == "tex") return M_TEX;
	if (m == "free") return M_FREE;
	return M_NONE;
}

const char *modeName(int m) {
	static const char *names[] = { "none", "page", "line", "vn", "tex", "free" };
	return names[m];
}

struct VoiceInfo {
	VVal voice, strVoice, incVoice, noplayvoice;
	bool unknown = false; // set from an & entity: whatever the compiler works out
};

struct Voices {
	std::map<std::string, VoiceInfo> map;
	VVal def;
	std::vector<VEntry> next;
	bool nextUnknown = false;
};

struct Pending {
	bool set = false;
	OStr name, disp;
	std::vector<VEntry> voice;
	bool voiceUnknown = false;
};

struct TextState {
	int mode = M_NONE;
	bool crAfterName = false, noCrOnce = false, noErOnce = false, afterPage = true, doLine = false;
	bool emptyLine = true, commandLine = false, prevEmptyLine = true, afterName = false;
	Pending pending;
	OStr lastName, lastDisp;
	Voices voices;
};

struct Msg {
	bool open = false, visible = false, cont = false;
	OStr name, disp;
	std::vector<VEntry> voice;
	bool voiceUnknown = false;
	std::string text;
};

struct Frame {
	TextState pre;
	std::vector<TextState> falls;
	bool hasElse = false;
};

using Attrs = std::vector<std::pair<std::string, std::string>>;

OStr attrOf(const Attrs &a, const std::string &name) {
	for (const auto &p : a) {
		if (p.first == name) return p.second;
	}
	return std::nullopt;
}

VVal vattr(const Attrs &a, const std::string &name) {
	OStr v = attrOf(a, name);
	return v ? VVal::string(*v) : VVal();
}

class Model {
public:
	TextState s;
	Msg msg;
	bool dead = false;
	int deadLevel = 0, deadNest = 0;
	std::vector<Frame> ifs;
	bool globalIgnoreCR = true;
	std::vector<Event> out;

	bool ignoreCR() const { return s.mode == M_NONE ? globalIgnoreCR : false; }

	void setLineMode(const std::string &m) {
		s.mode = modeOf(m);
		s.afterPage = true;
		s.doLine = false;
		s.emptyLine = true;
		s.commandLine = false;
		s.prevEmptyLine = true;
		s.afterName = false;
	}

	void setCrAfterName(const OStr &m) {
		s.crAfterName = m && *m != "false" && *m != "0" && !m->empty();
		s.afterName = false;
	}

	// --- messages

	void pushStep(const Value &step) {
		Event e;
		e.step = step;
		out.push_back(e);
	}

	void emitStep(const Value &step) {
		closeMessage(F_NOWAIT);
		pushStep(step);
	}

	void textName(const OStr &name, const OStr &disp, const std::vector<VEntry> &voice, bool voiceUnknown) {
		closeMessage(F_NOWAIT);
		s.pending.set = true;
		s.pending.name = name && !name->empty() ? name : std::nullopt;
		s.pending.disp = disp && !disp->empty() ? disp : std::nullopt;
		s.pending.voice = voice;
		s.pending.voiceUnknown = voiceUnknown;
	}

	void openMessage() {
		if (msg.open) return;
		msg = Msg();
		msg.open = true;
		if (s.pending.set) {
			msg.name = s.pending.name;
			msg.disp = s.pending.disp;
			msg.voice = s.pending.voice;
			msg.voiceUnknown = s.pending.voiceUnknown;
			s.pending = Pending();
		} else {
			msg.name = s.lastName;
			msg.disp = s.lastDisp;
			msg.cont = true;
		}
	}

	static std::string escapeText(const std::string &c) {
		if (c == "\\") return "\\\\";
		if (c == "%") return "\\%";
		if (c == "[") return "\\[";
		return c;
	}

	void textCh(const std::string &c) {
		openMessage();
		msg.text += escapeText(c);
		msg.visible = true;
	}

	void textR() {
		openMessage();
		msg.text += "\n";
	}

	void textRuby(const std::string &t) {
		openMessage();
		msg.text += "[" + t + "]";
	}

	void textInlineWait() {
		if (msg.open && msg.visible) msg.text += "\\k";
		else emitStep(makeStep({ strValue("l") }));
	}

	void textWait(int flag, const Value *elmStep) {
		if (msg.open && msg.visible) {
			closeMessage(flag);
			return;
		}
		closeMessage(F_NOWAIT);
		if (!(flag & F_LINE) && s.noErOnce) {
			s.noErOnce = false;
			emitStep(makeStep({ strValue("*noer") }));
		}
		if (elmStep != nullptr) {
			emitStep(*elmStep);
		} else if (flag & F_LINE) {
			emitStep(makeStep({ strValue("l"), intValue(0), strValue("voicewait"), strValue("true") }));
			emitStep(makeStep({ strValue("afterline") }));
		} else {
			emitStep(makeStep({ strValue("p"), intValue(0), strValue("nosave"), strValue("true") }));
		}
	}

	void closeMessage(int flag) {
		if (!msg.open) return;
		Msg m = msg;
		msg = Msg();
		if (!m.visible) {
			for (char c : m.text) {
				if (c == '\n') pushStep(makeStep({ strValue("r") }));
			}
			return;
		}
		if (m.cont) flag |= F_CONT;
		if (!(flag & (F_LINE | F_NOWAIT)) && s.noErOnce) {
			flag |= F_NOER;
			s.noErOnce = false;
		}
		Event e;
		e.isText = true;
		e.name = m.name;
		e.disp = m.disp;
		e.text = m.text;
		e.voice = m.voice;
		e.voiceUnknown = m.voiceUnknown;
		e.flag = flag;
		out.push_back(e);
		s.lastName = m.name;
		s.lastDisp = m.disp;
	}

	// --- the line modes

	void lineCheck() {
		if (s.doLine) {
			parseL();
			s.doLine = false;
		}
	}

	void emptyCheck() {
		switch (s.mode) {
		case M_LINE: case M_VN: case M_FREE:
			if (s.afterPage) s.afterPage = false;
			else if (!s.afterName) textR();
			break;
		case M_TEX:
			if (s.afterPage) s.afterPage = false;
			break;
		default:
			break;
		}
		if (!s.afterName && (s.mode <= M_LINE || s.prevEmptyLine)) {
			textName(std::nullopt, std::nullopt, {}, false);
			s.afterName = true;
		}
	}

	void parseCh(const std::string &t) {
		if (s.emptyLine) {
			lineCheck();
			emptyCheck();
		}
		textCh(t);
		s.emptyLine = false;
		s.afterPage = false;
		s.afterName = false;
	}

	void parseL() { textWait(F_LINE, nullptr); }

	void parseP() {
		textWait(0, nullptr);
		s.afterPage = true;
		s.emptyLine = true;
		s.commandLine = false;
	}

	void checkName() {
		if (s.emptyLine) {
			lineCheck();
			emptyCheck();
		}
		s.emptyLine = false;
		s.afterPage = false;
		s.afterName = false;
	}

	void parseR(bool eol) {
		if (!eol) {
			if (s.doLine) {
				parseL();
				s.doLine = false;
			}
			checkName();
			textR();
			return;
		}
		if (s.noCrOnce) {
			s.noCrOnce = false;
			s.prevEmptyLine = s.emptyLine;
			s.emptyLine = true;
			s.commandLine = false;
			return;
		}
		if (s.crAfterName && s.afterName) {
			s.commandLine = false;
			return;
		}
		switch (s.mode) {
		case M_PAGE:
			if (s.emptyLine) {
				if (!s.commandLine) s.prevEmptyLine = s.emptyLine;
			} else {
				s.prevEmptyLine = s.emptyLine;
				s.emptyLine = true;
				textWait(0, nullptr);
			}
			break;
		case M_LINE:
			if (s.emptyLine) {
				if (!s.commandLine) s.prevEmptyLine = s.emptyLine;
			} else {
				s.prevEmptyLine = s.emptyLine;
				s.emptyLine = true;
				parseL();
			}
			break;
		case M_VN:
			if (s.emptyLine) {
				if (!s.prevEmptyLine) {
					if (!s.commandLine) {
						s.doLine = false;
						s.prevEmptyLine = s.emptyLine;
						parseP();
					}
				} else {
					s.prevEmptyLine = s.emptyLine;
				}
			} else {
				s.prevEmptyLine = s.emptyLine;
				s.emptyLine = true;
				s.doLine = true;
			}
			break;
		case M_TEX: case M_FREE:
			if (s.emptyLine) {
				if (!s.commandLine) {
					if (!s.prevEmptyLine) {
						s.prevEmptyLine = s.emptyLine;
						parseP();
					} else {
						s.prevEmptyLine = s.emptyLine;
					}
				}
			} else {
				s.prevEmptyLine = s.emptyLine;
				s.emptyLine = true;
			}
			break;
		default:
			break;
		}
		s.commandLine = false;
	}

	// What a tag that isn't text does to the line modes before it runs
	void commandBlock() {
		if (s.mode != M_NONE) {
			if (s.doLine) {
				parseL();
				s.doLine = false;
			}
			s.commandLine = true;
		}
	}

	void markDead() {
		closeMessage(F_NOWAIT);
		dead = true;
		deadLevel = int(ifs.size());
		deadNest = 0;
	}

	// --- voices (MainWindow.tjs setVoice/getVoice, as SceneParser.tjs has them)

	VVal getVoice(const std::string &name, bool &unknown) {
		unknown = false;
		if (name.empty()) return VVal();
		auto it = s.voices.map.find(name);
		VoiceInfo *info = nullptr;
		if (it != s.voices.map.end()) {
			info = &it->second;
		} else if (!s.voices.def.isVoid()) {
			info = &s.voices.map[name];
			info->voice = s.voices.def;
		}
		VVal voice;
		if (info != nullptr) {
			if (info->unknown) {
				unknown = true;
				return VVal();
			}
			if (!info->strVoice.isVoid()) {
				if (!(info->strVoice.kind == VVal::Str && info->strVoice.s == "ignore")) voice = info->strVoice;
				info->strVoice = VVal();
			} else if (!info->voice.isVoid()) {
				voice = info->voice;
				info->incVoice = VVal::integer(1);
			}
			if (truthy(info->incVoice)) {
				double v = numOf(info->voice);
				if (v >= 0) info->voice = VVal::integer(int64_t(v) + 1);
				info->incVoice = VVal();
			}
		}
		return voice;
	}

	void setVoice(const std::string &name, const std::string &voice, const Attrs &elm) {
		VoiceInfo &info = s.voices.map[name];
		if (!voice.empty() && voice[0] == '@') {
			int64_t n = int64_t(tjsNumber(voice.substr(1)));
			if (!info.voice.isVoid()) info.voice = VVal::integer(int64_t(numOf(info.voice)) + n);
			else info.voice = VVal::integer(n);
		} else if (voice == "clear") {
			info.strVoice = VVal();
			info.voice = VVal();
			info.incVoice = VVal();
			info.unknown = false;
		} else if (voice == "dec") {
			if (!info.voice.isVoid()) info.voice = VVal::integer(int64_t(numOf(info.voice)) - 1);
		} else if (voice == "count" || voice == "inc") {
			if (!info.voice.isVoid()) info.voice = VVal::integer(int64_t(numOf(info.voice)) + 1);
		} else if (voice == "ignore") {
			info.strVoice = VVal::string("ignore");
		} else if (digitsOnly(voice)) {
			if (truthy(vattr(elm, "once")) || truthy(vattr(elm, "replace"))) info.strVoice = VVal::integer(int64_t(tjsNumber(voice)));
			else info.voice = VVal::integer(int64_t(tjsNumber(voice)));
		} else if (!voice.empty()) {
			info.strVoice = VVal::string(voice);
		}
		info.incVoice = VVal::integer(truthy(vattr(elm, "incvoice")) || truthy(vattr(elm, "replace")) ? 1 : 0);
		info.noplayvoice = vattr(elm, "noplayvoice");
	}

	void setNextVoice(const std::string &name) {
		bool unknown;
		VVal voice = getVoice(name, unknown);
		if (unknown) {
			s.voices.nextUnknown = true;
			return;
		}
		if (!voice.isVoid()) {
			VEntry e;
			e.name = name;
			e.voice = voice;
			auto it = s.voices.map.find(name);
			if (it != s.voices.map.end() && !it->second.noplayvoice.isVoid()) e.noplayvoice = it->second.noplayvoice;
			s.voices.next.push_back(e);
		}
	}

	void parseVoice(const Attrs &elm) {
		for (const auto &a : elm) {
			const std::string &n = a.first;
			if (n == "tagname" || n == "replace" || n == "once" || n == "entry" || n == "taglist") continue;
			if (n == "base") {
				emitStep(makeStep({ strValue("eval"), intValue(0), strValue("exp"),
									strValue("kag.voiceBase = \"" + a.second + "\"") }));
				continue;
			}
			if (n == "default") {
				s.voices.def = VVal::string(a.second);
				continue;
			}
			if (!n.empty() && n[0] == '_') continue;
			setVoice(n, a.second, elm);
		}
	}

	void parseParse() {
		s.voices.map.clear();
		s.voices.next.clear();
		s.voices.nextUnknown = false;
	}

	// The voices a World tag sets for its character (KAGEnvironment's voice= and nextvoice=).
	// Which tags are characters is envinit.tjs's to say: any tag with these attributes is
	// taken for one, which is harmless for the others (no 【】 line has their name)
	void worldVoice(const std::string &tag, const Attrs &e, const std::vector<std::string> &entities) {
		static const char *kagTags[] = { "dispname", "playvoice", "stopvoice", "voice", "nextvoice", "l", "p" };
		for (const char *k : kagTags) {
			if (tag == k) return;
		}
		auto isEntity = [&](const std::string &n) {
			for (const auto &x : entities) if (x == n) return true;
			return false;
		};
		OStr voice = attrOf(e, "voice");
		if (voice) {
			if (isEntity("voice")) s.voices.map[tag].unknown = true;
			else setVoice(tag, *voice, e);
		}
		OStr nv = attrOf(e, "nextvoice");
		if (nv) {
			if (isEntity("nextvoice")) {
				s.voices.map[tag].unknown = true;
			} else if (!nv->empty() && *nv != "true" && *nv != "false" && *nv != "1" && *nv != "0") {
				setVoice(tag, *nv, e);
			}
			setNextVoice(tag);
		}
	}

	// --- [if]

	void endBranch(Frame &f) {
		closeMessage(F_NOWAIT);
		if (!dead) f.falls.push_back(s);
		dead = false;
		deadNest = 0;
	}

	void control(const std::string &name, const Value &step) {
		if (dead) {
			if (name == "if") {
				deadNest++;
				return;
			}
			if (name == "elsif" || name == "else") {
				if (deadNest > 0 || deadLevel == 0 || int(ifs.size()) != deadLevel) return;
			}
			if (name == "endif") {
				if (deadNest > 0) {
					deadNest--;
					return;
				}
				if (deadLevel == 0 || int(ifs.size()) != deadLevel) return;
			}
		}
		if (name == "if") {
			emitStep(step);
			Frame f;
			f.pre = s;
			ifs.push_back(f);
		} else if (ifs.empty()) {
			return; // [elsif]/[else]/[endif] without [if]: the compiler stops; the emitter doesn't write one
		} else if (name == "elsif" || name == "else") {
			Frame &f = ifs.back();
			endBranch(f);
			s = f.pre;
			emitStep(step);
			if (name == "else") f.hasElse = true;
		} else if (name == "endif") {
			Frame &fr = ifs.back();
			endBranch(fr);
			if (!fr.hasElse) fr.falls.push_back(fr.pre);
			Frame f = fr;
			ifs.pop_back();
			emitStep(step);
			if (f.falls.empty()) {
				s = f.pre;
				dead = true;
				deadLevel = int(ifs.size());
				deadNest = 0;
				return;
			}
			s = f.falls[0];
		}
	}

	// --- what each thing written does (SceneParser.processTag and the tokenizer)

	void chars(const std::string &text) {
		for (const std::string &c : utf8Chars(text)) chTag(c);
	}

	void chTag(const std::string &c) {
		if (dead) return;
		if (s.mode == M_NONE) textCh(c);
		else parseCh(c);
	}

	// 【name/display name】, where a line starts (emptyLine)
	void name(const std::string &n, const OStr &d) {
		if (dead) return;
		if (s.mode == M_NONE || !s.emptyLine) {
			chars("【" + n + (d ? "/" + *d : "") + "】");
			return;
		}
		lineCheck();
		setNextVoice(n);
		std::vector<VEntry> voices;
		bool unknown = s.voices.nextUnknown;
		if (!s.voices.next.empty() || unknown) {
			voices = s.voices.next;
			s.voices.next.clear();
			s.voices.nextUnknown = false;
		}
		textName(n, d ? d : OStr(""), voices, unknown);
		s.afterName = true;
	}

	void r() {
		if (dead) return;
		if (s.mode == M_NONE) textR();
		else parseR(false);
	}

	void eol() {
		if (ignoreCR() || dead) return;
		if (s.mode == M_NONE) textR();
		else parseR(true);
	}

	void l(const Value &step) {
		if (dead) return;
		if (s.mode == M_NONE) textWait(F_LINE, &step);
		else textInlineWait();
	}

	void p(const Value &step) {
		if (dead) return;
		if (s.mode == M_NONE) textWait(0, &step);
		else parseP();
	}

	void noer() {
		if (dead) return;
		s.noErOnce = true;
	}

	void ruby(const std::string &t) {
		if (dead) return;
		if (s.mode == M_NONE) textRuby(t);
		else {
			checkName();
			textRuby(t);
		}
	}

	// a tag that becomes a step (SceneParser.command)
	void command(const Value &step, const Attrs &attrs, const std::vector<std::string> &entities, bool hasCond) {
		if (dead) return;
		commandBlock();
		std::string tag = step.list->empty() ? "" : scalarText((*step.list)[0]);
		if (tag == "linemode") setLineMode(attrOf(attrs, "mode").value_or(""));
		if (tag == "craftername") setCrAfterName(attrOf(attrs, "mode"));
		emitStep(step);
		if (!hasCond) worldVoice(tag, attrs, entities);
	}

	// [voice], [nextvoice], [parse]: no step
	void voiceTag(const Attrs &attrs) {
		if (dead) return;
		commandBlock();
		parseVoice(attrs);
	}

	void nextVoiceTag(const std::string &name) {
		if (dead) return;
		commandBlock();
		setNextVoice(name);
	}

	void parseTag() {
		if (dead) return;
		commandBlock();
		parseParse();
	}

	// [seladd]: a choice, no step
	void seladd() {
		if (dead) return;
		commandBlock();
	}

	// [select]: the scene ends with its choices
	void select() {
		if (dead) return;
		commandBlock();
		closeMessage(F_NOWAIT);
		markDead();
	}

	// a step the compiler adds before the line modes see the tag ([emb], a text tag with
	// cond=, a [jump] that's a step, [iscript])
	void early(const Value &step, bool deadAfter) {
		if (dead) return;
		emitStep(step);
		if (deadAfter) markDead();
	}

	// a [jump] that ends the scene
	void jumpEnd() {
		if (dead) return;
		closeMessage(F_NOWAIT);
		markDead();
	}

	void label() {
		closeMessage(F_NOWAIT);
		dead = false;
		deadNest = 0;
	}
};

// ------------------------------------------------------------------------
// Writing tags

// An attribute value as a tag takes it. entity: the value is an expression (&)
std::string attrValue(const std::string &v, bool entity) {
	std::string body;
	bool quote = v.empty();
	for (size_t i = 0; i < v.size(); i++) {
		char c = v[i];
		if (c == ' ' || c == '\t' || c == ']') quote = true;
		if (c == '"' || c == '`') {
			body += '`';
			quote = true;
		} else if (i == 0 && !entity && (c == '&' || c == '%')) {
			body += '`'; // a literal & or % at the start
		} else if (i == 0 && c == '\'') {
			quote = true;
		}
		body += c;
	}
	if (entity) body = "&" + body;
	return quote ? "\"" + body + "\"" : body;
}

bool validAttrName(const std::string &n) {
	if (n.empty() || n == "cond" || n == "*") return false;
	for (char c : n) {
		if (c == ' ' || c == '\t' || c == '=' || c == ']' || c == '[' || c == '"' || c == '\'' || c == '`') return false;
		if (c >= 'A' && c <= 'Z') return false; // the tokenizer lower-cases names
	}
	return true;
}

// A step [tagname, flags..., 0, name, value...] as its tag. Fills the attributes as the
// compiler takes them (entities kept as expressions, listed in `entities`), and *cond/*mp
struct StepParts {
	std::string tag;
	std::vector<std::string> flags;
	Attrs attrs;                    // name, value as written (entity: the expression)
	std::vector<std::string> entities;
	OStr cond;
	const Value *mp = nullptr;
	bool ok = true;
	std::string problem;
};

StepParts splitStep(const Value &step) {
	StepParts p;
	if (step.type != VT::List || step.list->empty() || (*step.list)[0].type != VT::String) {
		p.ok = false;
		p.problem = "a step that isn't a tag";
		return p;
	}
	const auto &l = *step.list;
	p.tag = l[0].s;
	size_t i = 1;
	for (; i < l.size(); i++) {
		if (l[i].type == VT::Int && l[i].i == 0) {
			i++;
			break;
		}
		p.flags.push_back(scalarText(l[i]));
	}
	for (; i + 1 < l.size(); i += 2) {
		std::string name = scalarText(l[i]);
		const Value &v = l[i + 1];
		if (name == "*cond") {
			p.cond = scalarText(v);
			continue;
		}
		if (name == "*mp") {
			p.mp = &v;
			continue;
		}
		if (v.type == VT::String && !v.s.empty() && v.s[0] == '&') {
			p.entities.push_back(name);
			p.attrs.push_back({ name, v.s.substr(1) });
		} else if (v.type == VT::String && !v.s.empty() && v.s[0] == '`') {
			p.attrs.push_back({ name, v.s.substr(1) });
		} else if (v.type == VT::List || v.type == VT::Object) {
			p.ok = false;
			p.problem = "the attribute " + name + " holds " + shortJson(v);
			p.attrs.push_back({ name, "" });
		} else {
			p.attrs.push_back({ name, scalarText(v) });
		}
	}
	if (i < l.size()) {
		p.ok = false;
		p.problem = "an attribute without a value";
	}
	return p;
}

bool isEntity(const StepParts &p, const std::string &name) {
	for (const auto &e : p.entities) if (e == name) return true;
	return false;
}

std::string tagSource(const StepParts &p, const std::string &tagName, bool withCond = true) {
	std::string s = "[" + tagName;
	for (const auto &f : p.flags) s += " " + f;
	for (const auto &a : p.attrs) s += " " + a.first + "=" + attrValue(a.second, isEntity(p, a.first));
	if (withCond && p.cond) s += " cond=" + attrValue(*p.cond, false);
	return s + "]";
}

// The attributes as the World gets them (flags are "true")
Attrs worldAttrs(const StepParts &p) {
	Attrs a;
	for (const auto &f : p.flags) a.push_back({ f, "true" });
	for (const auto &x : p.attrs) a.push_back(x);
	return a;
}

// A step that comes from a macro using mp: the macro is written again, with the arguments
// it had, so mp is the same when it runs (the macro's name is scn2ks_mp)
std::string withMacro(const std::string &body, const Value *mp, std::string *problem) {
	std::string call = "[scn2ks_mp";
	if (mp != nullptr && mp->type == VT::Object) {
		std::vector<std::pair<std::string, Value>> args(mp->object->begin(), mp->object->end());
		std::sort(args.begin(), args.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
		for (const auto &m : args) {
			if (m.first == "tagname" || m.first == "taglist") continue;
			if (!validAttrName(m.first)) {
				if (problem) *problem = "the macro argument " + m.first + " can't be written";
				continue;
			}
			std::string v = scalarText(m.second);
			bool ent = !v.empty() && v[0] == '&';
			call += " " + m.first + "=" + attrValue(ent ? v.substr(1) : v, ent);
		}
	}
	return "[macro name=scn2ks_mp]" + body + "[endmacro]" + call + "]";
}

// ------------------------------------------------------------------------
// What the emitter writes: tokens, each with its source and what it does in the Model

struct Tok {
	enum Kind { Chars, ChTag, Name, R, L, P, Noer, Ruby, Command, VoiceTag, NextVoice, Parse, Seladd, Select,
				Early, Control, Script, JumpEnd, Eol, Cont, Blank } kind;
	std::string a;   // Chars/ChTag/Ruby: the text; Name: the name; Control: if/elsif/else/endif
	OStr b;          // Name: the display name
	std::string src; // the source for tags (empty: the default)
	Value step;      // the step it makes
	Attrs attrs;     // Command/VoiceTag: attributes
	std::vector<std::string> entities;
	bool flag = false; // Command: has cond; Early: dead after
};

Tok tok(Tok::Kind k, const std::string &a = std::string()) {
	Tok t{ k, a, std::nullopt, std::string(), Value(), Attrs(), {}, false };
	return t;
}

// The physical lines written, and the one being written
struct Sink {
	std::vector<std::string> lines;
	std::string cur;
	bool record = true;

	void newline() {
		if (record) lines.push_back(cur);
		cur.clear();
	}
};

// Writes a token and runs it in the model
void apply(const Tok &t, Model &m, Sink &out) {
	switch (t.kind) {
	case Tok::Chars: {
		std::vector<std::string> cs = utf8Chars(t.a);
		for (size_t i = 0; i < cs.size(); i++) {
			const std::string &c = cs[i];
			bool lineStart = out.cur.empty();
			if (c == "[") {
				out.cur += "[[";
			} else if (c == "\\" || c == "\t" || c == "\r" || (lineStart && (c == ";" || c == "*" || c == "@"))) {
				out.cur += "[ch text=" + attrValue(c, false) + "]";
			} else {
				out.cur += c;
			}
			m.chTag(c);
		}
		break;
	}
	case Tok::ChTag:
		out.cur += "[ch text=" + attrValue(t.a, false) + "]";
		m.chTag(t.a);
		break;
	case Tok::Name:
		out.cur += "【" + t.a + (t.b ? "/" + *t.b : std::string()) + "】";
		m.name(t.a, t.b);
		break;
	case Tok::R:
		out.cur += "[r]";
		m.r();
		break;
	case Tok::L:
		out.cur += t.src.empty() ? "[l]" : t.src;
		m.l(t.step.type == VT::List ? t.step : makeStep({ strValue("l") }));
		break;
	case Tok::P:
		out.cur += t.src.empty() ? "[p]" : t.src;
		m.p(t.step.type == VT::List ? t.step : makeStep({ strValue("p") }));
		break;
	case Tok::Noer:
		out.cur += "[noer]";
		m.noer();
		break;
	case Tok::Ruby:
		out.cur += "[ruby text=" + attrValue(t.a, false) + "]";
		m.ruby(t.a);
		break;
	case Tok::Command:
		out.cur += t.src;
		m.command(t.step, t.attrs, t.entities, t.flag);
		break;
	case Tok::VoiceTag:
		out.cur += t.src;
		m.voiceTag(t.attrs);
		break;
	case Tok::NextVoice:
		out.cur += t.src;
		m.nextVoiceTag(t.a);
		break;
	case Tok::Parse:
		out.cur += "[parse]";
		m.parseTag();
		break;
	case Tok::Seladd:
		out.cur += t.src;
		m.seladd();
		break;
	case Tok::Select:
		out.cur += t.src;
		m.select();
		break;
	case Tok::Early:
		out.cur += t.src;
		m.early(t.step, t.flag);
		break;
	case Tok::Control:
		out.cur += t.src;
		m.control(t.a, t.step);
		break;
	case Tok::Script: {
		// on lines of its own: the block, then nothing else on its last line
		if (!out.cur.empty()) out.newline();
		out.cur = "[iscript]";
		out.newline();
		std::string code = t.a;
		size_t pos = 0;
		while (pos < code.size()) {
			size_t nl = code.find('\n', pos);
			if (nl == std::string::npos) nl = code.size();
			out.cur = code.substr(pos, nl - pos);
			out.newline();
			pos = nl + 1;
		}
		out.cur = "[endscript]";
		out.newline();
		m.early(t.step, false);
		break;
	}
	case Tok::JumpEnd:
		out.cur += t.src;
		m.jumpEnd();
		break;
	case Tok::Eol: {
		// a line ending in [p] doesn't end with a new line (the tokenizer)
		bool skip = out.cur.size() >= 3 && out.cur.compare(out.cur.size() - 3, 3, "[p]") == 0;
		if (!skip) m.eol();
		out.newline();
		break;
	}
	case Tok::Cont:
		if (!m.ignoreCR()) out.cur += "\\";
		out.newline();
		break;
	case Tok::Blank:
		if (!out.cur.empty()) {
			bool skip = out.cur.size() >= 3 && out.cur.compare(out.cur.size() - 3, 3, "[p]") == 0;
			if (!skip) m.eol();
			out.newline();
		}
		m.eol();
		out.newline();
		break;
	}
}

// ------------------------------------------------------------------------
// The file's scenes

struct TScene {
	std::string label, title;
	const Value *texts = nullptr;
	std::vector<const Value *> items; // the lines, but save points and read flags
	const Value *nexts = nullptr;
	const Value *raw = nullptr;
	std::vector<Event> target;        // the texts and steps, in order
	std::vector<int> itemTarget;      // for each item, its index in target
};

bool isSavePoint(const Value &v) {
	return v.type == VT::List && !v.list->empty() && isInt(&(*v.list)[0]);
}

class Emitter {
public:
	Emitter(const Options &o, std::vector<std::string> &n) : opt(o), notes(n) {}

	Sink sink;
	Model model;
	const Options &opt;
	std::vector<std::string> &notes;
	std::vector<std::string> sceneNotes;
	std::string where;
	int textCount = 0;

	// the scene's texts and steps, and how many of them have come out
	std::vector<Event> target;
	size_t produced = 0;

	void note(const std::string &what) {
		std::string n = where + ": " + what;
		for (const auto &x : sceneNotes) if (x == n) return;
		sceneNotes.push_back(n);
	}

	// Whether tokens written now would compile to what comes next in the file. openText:
	// the target text a message they leave open should be
	bool fits(const std::vector<Tok> &toks, const Event *openText = nullptr, size_t need = 0) {
		Model m = model;
		Sink s = sink;
		s.record = false;
		size_t before = m.out.size();
		for (const Tok &t : toks) apply(t, m, s);
		size_t n = m.out.size() - before;
		if (n < need || produced + n > target.size()) return false;
		for (size_t i = 0; i < n; i++) {
			if (!sameEvent(m.out[before + i], target[produced + i], nullptr)) return false;
		}
		if (openText != nullptr && m.msg.open && m.msg.visible) {
			// what's open is the text being written: all but how it ends must be right
			Event e;
			e.isText = true;
			e.name = m.msg.name;
			e.disp = m.msg.disp;
			e.text = m.msg.text;
			e.voice = m.msg.voice;
			e.voiceUnknown = m.msg.voiceUnknown;
			e.flag = (m.msg.cont ? F_CONT : 0) | (openText->flag & ~F_CONT);
			if (!sameEvent(e, *openText, nullptr)) return false;
		}
		return true;
	}

	// Writes tokens, checking what comes out against the file
	void commit(const std::vector<Tok> &toks) {
		size_t before = model.out.size();
		for (const Tok &t : toks) apply(t, model, sink);
		check(before);
	}

	void check(size_t before) {
		for (size_t i = before; i < model.out.size(); i++) {
			const Event &got = model.out[i];
			if (produced >= target.size()) {
				note(got.isText ? "an extra text comes out" : "an extra step comes out: " + shortJson(got.step));
				continue;
			}
			std::string why;
			if (sameEvent(got, target[produced], &why)) {
				produced++;
				continue;
			}
			note(why);
			const Event &want = target[produced];
			if (got.isText == want.isText) {
				produced++;
			} else if (got.isText) {
				// steps the file has before this text didn't come out
				while (produced < target.size() && !target[produced].isText) produced++;
				if (produced < target.size()) produced++;
			}
			// an extra step: the file's next one is still to come
		}
	}

	// Tries the candidates in turn, writing the first that fits (or the first, noting it)
	bool choose(const std::vector<std::vector<Tok>> &cands, const Event *openText = nullptr, size_t need = 0) {
		for (const auto &c : cands) {
			if (fits(c, openText, need)) {
				commit(c);
				return true;
			}
		}
		if (!cands.empty()) commit(cands[0]);
		return false;
	}

	// Ends the line being written without changing what's compiled: a new line where the line
	// modes take it as nothing, otherwise \ (which joins the next line to it)
	void breakLine() {
		if (sink.cur.empty()) return;
		if (model.ignoreCR()) {
			commit({ tok(Tok::Eol) });
			return;
		}
		Model m = model;
		Sink s = sink;
		s.record = false;
		apply(tok(Tok::Eol), m, s);
		// (prevEmptyLine only counts in the modes where an empty line is a [p])
		bool same = m.out.size() == model.out.size() && m.s.emptyLine == model.s.emptyLine && m.s.doLine == model.s.doLine &&
					m.msg.text == model.msg.text && m.msg.open == model.msg.open &&
					(model.s.mode <= M_LINE || m.s.prevEmptyLine == model.s.prevEmptyLine);
		commit({ tok(same ? Tok::Eol : Tok::Cont) });
	}

	void comment(const std::string &text) {
		breakLine();
		sink.cur = "; " + text;
		sink.newline();
	}

	// --- texts

	// The text of a message as tokens (the player's escapes, scn.txt)
	std::vector<Tok> textTokens(const std::string &t) {
		std::vector<Tok> r;
		std::string run;
		auto flush = [&]() {
			if (!run.empty()) r.push_back(tok(Tok::Chars, run));
			run.clear();
		};
		for (size_t i = 0; i < t.size();) {
			char c = t[i];
			if (c == '\\') {
				if (i + 1 >= t.size()) {
					note("a text ends with a lone \\ (left out)");
					i++;
					continue;
				}
				char d = t[i + 1];
				i += 2;
				if (d == '\\') {
					flush();
					r.push_back(tok(Tok::ChTag, "\\"));
				} else if (d == '%' || d == '[') {
					run += d;
				} else if (d == 'k') {
					flush();
					r.push_back(tok(Tok::L));
				} else {
					note(std::string("a text has the unknown escape \\") + d + " (left out)");
				}
				continue;
			}
			if (c == '\n') {
				flush();
				r.push_back(tok(Tok::R));
				i++;
				continue;
			}
			if (c == '%') {
				// a font command, in files compiled by Wamsoft's tools: a tag here
				flush();
				size_t j = i + 1;
				while (j < t.size() && t[j] >= '0' && t[j] <= '9') j++;
				Tok f = tok(Tok::Command);
				if (j > i + 1) {
					std::string size = t.substr(i + 1, j - i - 1);
					std::string v = std::stoll(size) ? std::to_string(std::stoll(size)) : "default";
					f.step = makeStep({ strValue("font"), intValue(0), strValue("size"), strValue(v) });
					f.attrs = { { "size", v } };
					f.src = "[font size=" + v + "]";
					i = j;
				} else if (j < t.size() && t[j] == 'r') {
					f.step = makeStep({ strValue("resetfont") });
					f.src = "[resetfont]";
					i = j + 1;
				} else {
					note("a text has an unknown % command (left out)");
					i = j < t.size() ? j + 1 : j;
					continue;
				}
				note("a text has a font command, written as a tag, which splits the text in two");
				r.push_back(f);
				continue;
			}
			if (c == '[') {
				size_t e = t.find(']', i + 1);
				if (e == std::string::npos) {
					note("a text has a [ without its ] (written as text)");
					run += '[';
					i++;
					continue;
				}
				flush();
				r.push_back(tok(Tok::Ruby, t.substr(i + 1, e - i - 1)));
				i = e + 1;
				continue;
			}
			run += c;
			i++;
		}
		flush();
		return r;
	}

	// The tags that make the compiler give a 【name】 line these voices (corrections only:
	// what the tags written so far give is left alone when it's right)
	std::vector<Tok> voiceFix(const std::string &name, const Event &want) {
		std::vector<Tok> r;
		Model m = model;
		Sink s;
		s.record = false;
		auto predict = [&](Model mm, bool &unknown) {
			mm.setNextVoice(name);
			unknown = mm.s.voices.nextUnknown;
			return mm.s.voices.next;
		};
		bool unknown;
		std::vector<VEntry> got = predict(m, unknown);
		if (!unknown && got == want.voice) return r;

		const std::vector<VEntry> &T = want.voice;
		bool hasOwn = !T.empty() && T.back().name == name;
		std::vector<VEntry> others(T.begin(), T.end() - (hasOwn ? 1 : 0));
		auto canWrite = [&](const std::string &n) {
			return validAttrName(n) && n != "tagname" && n != "replace" && n != "once" && n != "entry" && n != "taglist" &&
				   n != "base" && n != "default" && n != "noplayvoice" && n != "incvoice" && n[0] != '_';
		};
		auto add = [&](Tok t) {
			apply(t, m, s);
			r.push_back(t);
		};
		// [voice name=voice], setting its counter (once: for the next line only)
		auto voiceTag = [&](const VEntry &e, bool once) {
			Tok t = tok(Tok::VoiceTag);
			std::string v = e.voice.text();
			if (e.voice.kind == VVal::Str && (digitsOnly(v) || v == "clear" || v == "dec" || v == "count" || v == "inc" ||
											  v == "ignore" || v.empty() || v[0] == '@'))
				note("the voice " + v + " of " + e.name + " can't be given by a tag");
			if (e.voice.kind == VVal::Int && e.voice.i < 0) note("the voice " + v + " of " + e.name + " can't be given by a tag");
			if (!canWrite(e.name)) note("the voice of " + e.name + " can't be given by a tag (its name)");
			t.attrs = { { e.name, v } };
			t.src = "[voice " + e.name + "=" + attrValue(v, false);
			if (once) {
				t.attrs.push_back({ "once", "true" });
				t.src += " once";
			}
			if (!e.noplayvoice.isVoid()) {
				t.attrs.push_back({ "noplayvoice", e.noplayvoice.text() });
				t.src += " noplayvoice=" + attrValue(e.noplayvoice.text(), false);
			}
			t.src += "]";
			return t;
		};
		auto nextVoice = [&](const std::string &n) {
			Tok t = tok(Tok::NextVoice, n);
			t.src = "[nextvoice name=" + attrValue(n, false) + "]";
			return t;
		};
		// what the tags give that voice (counters move on as the compiler moves them)
		auto gives = [&](const std::vector<Tok> &toks, const std::string &n, const VEntry *want1) {
			Model mm = m;
			Sink ss;
			ss.record = false;
			for (const Tok &t : toks) apply(t, mm, ss);
			bool unk;
			VVal v = mm.getVoice(n, unk);
			auto info = mm.s.voices.map.find(n);
			VVal np = info != mm.s.voices.map.end() ? info->second.noplayvoice : VVal();
			if (want1 == nullptr) return !unk && v.isVoid();
			return !unk && v == want1->voice && np == want1->noplayvoice;
		};

		const std::vector<VEntry> &cur = m.s.voices.next;
		bool prefix = !m.s.voices.nextUnknown && cur.size() <= others.size();
		for (size_t i = 0; prefix && i < cur.size(); i++) prefix = cur[i] == others[i];
		bool anyUnknown = false;
		for (const auto &e : T) {
			auto it = m.s.voices.map.find(e.name);
			if (it != m.s.voices.map.end() && it->second.unknown) anyUnknown = true;
		}
		auto it = m.s.voices.map.find(name);
		if (it != m.s.voices.map.end() && it->second.unknown) anyUnknown = true;
		size_t start = cur.size();
		if (!prefix || anyUnknown) {
			add(tok(Tok::Parse));
			start = 0;
		}
		for (size_t i = start; i < others.size(); i++) {
			const VEntry &e = others[i];
			if (gives({}, e.name, &e)) {
				add(nextVoice(e.name));
			} else if (e.voice.kind == VVal::Int && gives({ voiceTag(e, false) }, e.name, &e)) {
				add(voiceTag(e, false));
				add(nextVoice(e.name));
			} else {
				add(voiceTag(e, true));
				add(nextVoice(e.name));
			}
		}
		// the line's own voice
		if (hasOwn) {
			const VEntry &e = T.back();
			if (!gives({}, name, &e)) {
				if (e.voice.kind == VVal::Int && gives({ voiceTag(e, false) }, name, &e)) add(voiceTag(e, false));
				else add(voiceTag(e, true));
			}
		} else if (!gives({}, name, nullptr)) {
			Tok t = tok(Tok::VoiceTag);
			t.attrs = { { name, "ignore" } };
			t.src = "[voice " + name + "=ignore]";
			if (!canWrite(name)) note("the voice of " + name + " can't be given by a tag (its name)");
			add(t);
		}
		return r;
	}

	// Whether the first character written at the start of a message would be taken for
	// something else
	void checkFirst(const std::vector<Tok> &body, bool lineStart) {
		if (body.empty() || body[0].kind != Tok::Chars) return;
		if (lineStart && model.s.mode != M_NONE && body[0].a.compare(0, 3, "【") == 0)
			note("a text starts with 【, which the script would take for a name");
	}

	void emitText(const Event &t, const Event *next) {
		textCount++;
		if (t.flag & F_NOTEXT) note("text " + std::to_string(t.id) + " is a voice without text, which a script can't say (written as text)");
		std::string body = t.text;
		bool cont = (t.flag & F_CONT) != 0;
		int mode = model.s.mode;
		std::vector<std::vector<Tok>> starts;
		if (mode != M_NONE && model.s.emptyLine && !sink.cur.empty()) breakLine();

		if (cont) {
			if (mode == M_NONE || !model.s.emptyLine) {
				starts.push_back({});
			} else if (!body.empty() && body[0] == '\n') {
				// a line that carries on the message (vn, free): the line break says the \n
				starts.push_back({ tok(Tok::Blank) }); // marker for that
				starts.push_back({});
			} else {
				starts.push_back({});
			}
		} else {
			if (mode == M_NONE) {
				starts.push_back({});
			} else {
				if (!model.s.emptyLine) {
					// the line before isn't over: only its end would start a message here
					starts.push_back({});
				} else {
					if (!sink.cur.empty()) breakLine();
					bool named = t.name || t.disp || !t.voice.empty();
					std::vector<Tok> fix = voiceFix(t.name.value_or(""), t);
					Tok nm = tok(Tok::Name, t.name.value_or(""));
					if (t.disp) nm.b = t.disp;
					if (t.name && (t.name->find('/') != std::string::npos || t.name->find("】") != std::string::npos))
						note("the name " + *t.name + " can't be written in 【】");
					if (!named && fix.empty()) {
						starts.push_back({});
						starts.push_back({ nm });
					} else {
						std::vector<Tok> s = fix;
						s.push_back(nm);
						starts.push_back(s);
					}
				}
			}
		}

		// how it ends
		std::vector<std::vector<Tok>> ends;
		int f = t.flag;
		if (f & F_NOWAIT) {
			ends.push_back({});
		} else if (f & F_LINE) {
			if (mode == M_NONE) ends.push_back({ tok(Tok::L) });
			else if (mode == M_LINE || mode == M_VN) ends.push_back({ tok(Tok::Eol) });
			else ends.push_back({ tok(Tok::L) });
		} else {
			std::vector<Tok> pre;
			if (f & F_NOER) pre.push_back(tok(Tok::Noer));
			auto with = [&](std::vector<Tok> e) {
				std::vector<Tok> r = pre;
				r.insert(r.end(), e.begin(), e.end());
				return r;
			};
			switch (mode) {
			case M_NONE: ends.push_back(with({ tok(Tok::P) })); break;
			case M_PAGE: ends.push_back(with({ tok(Tok::Eol) })); ends.push_back(with({ tok(Tok::P), tok(Tok::Eol) })); break;
			case M_VN: ends.push_back(with({ tok(Tok::Eol), tok(Tok::Blank) })); ends.push_back(with({ tok(Tok::P), tok(Tok::Eol) })); break;
			default: ends.push_back(with({ tok(Tok::P), tok(Tok::Eol) })); break;
			}
		}
		(void)next;

		// try each start with each end ([noer] first before the text, then before its end)
		std::vector<std::vector<Tok>> cands;
		for (int noerAt = 0; noerAt < ((f & F_NOER) ? 2 : 1); noerAt++) {
			for (const auto &st : starts) {
				bool stripNl = st.size() == 1 && st[0].kind == Tok::Blank;
				std::vector<Tok> b = textTokens(stripNl ? body.substr(1) : body);
				for (const auto &e0 : ends) {
					std::vector<Tok> c, e = e0;
					if ((f & F_NOER) && noerAt == 0) {
						if (!e.empty() && e[0].kind == Tok::Noer) e.erase(e.begin());
						c.push_back(tok(Tok::Noer));
					}
					if (!stripNl) c.insert(c.end(), st.begin(), st.end());
					c.insert(c.end(), b.begin(), b.end());
					c.insert(c.end(), e.begin(), e.end());
					cands.push_back(c);
				}
			}
		}
		bool lineStart = sink.cur.empty() || !cands.empty();
		if (!cands.empty()) {
			std::vector<Tok> b = textTokens(body);
			checkFirst(b, lineStart && model.s.emptyLine);
		}
		if (!choose(cands, &t)) {
			if (cont && mode != M_NONE && model.s.emptyLine) note("text " + std::to_string(t.id) + " carries on the message window, which can't be said where it is");
			else if (!cont && mode == M_NONE) note("text " + std::to_string(t.id) + " starts a message, which line mode none can't say");
			else if (!cont && !model.s.emptyLine) note("text " + std::to_string(t.id) + " starts a message where the line before isn't over");
			else if ((f & F_LINE) && mode != M_NONE && mode != M_LINE && mode != M_VN) note("text " + std::to_string(t.id) + " ends with [l], which line mode " + std::string(modeName(mode)) + " can't say");
		}
	}

	// --- steps

	Event *peek(size_t idx) { return idx < target.size() ? &target[idx] : nullptr; }

	bool stepIs(const Event *e, std::initializer_list<const char *> shape) {
		if (e == nullptr || e->isText || e->step.type != VT::List) return false;
		const auto &l = *e->step.list;
		if (l.size() != shape.size()) return false;
		size_t i = 0;
		for (const char *s : shape) {
			const Value &v = l[i++];
			if (s == nullptr) {
				if (!(v.type == VT::Int && v.i == 0)) return false;
			} else if (!(v.type == VT::String && v.s == s)) {
				return false;
			}
		}
		return true;
	}

	// Places a tag: after text on the same line, otherwise on a line of its own
	void placeTag() {
		if (!sink.cur.empty() && model.s.emptyLine) breakLine();
	}

	static bool sameState(const Model &a, const Model &b) {
		const TextState &x = a.s, &y = b.s;
		return x.mode == y.mode && x.crAfterName == y.crAfterName && x.noCrOnce == y.noCrOnce && x.noErOnce == y.noErOnce &&
			   x.afterPage == y.afterPage && x.doLine == y.doLine && x.emptyLine == y.emptyLine &&
			   x.commandLine == y.commandLine && x.prevEmptyLine == y.prevEmptyLine && x.afterName == y.afterName &&
			   x.pending.set == y.pending.set && a.msg.open == b.msg.open && a.msg.visible == b.msg.visible &&
			   a.msg.text == b.msg.text && a.dead == b.dead;
	}

	static bool sameEvents(const std::vector<Event> &a, const std::vector<Event> &b) {
		if (a.size() != b.size()) return false;
		for (size_t i = 0; i < a.size(); i++) {
			if (!sameEvent(a[i], b[i], nullptr)) return false;
		}
		return true;
	}

	// Writes a tag after the text of the line being written, or on a line of its own where
	// that compiles the same (the text's line then ends where the tag's would have)
	void commitTag(const std::vector<Tok> &tagToks) {
		if (!sink.cur.empty() && !model.s.emptyLine && !model.ignoreCR()) {
			auto run = [&](const std::vector<Tok> &toks) {
				Model m = model;
				Sink s = sink;
				s.record = false;
				for (const Tok &t : toks) apply(t, m, s);
				return m;
			};
			std::vector<Tok> same = tagToks, own = { tok(Tok::Eol) };
			same.push_back(tok(Tok::Eol));
			own.insert(own.end(), tagToks.begin(), tagToks.end());
			Model a = run(same), b = run(own);
			if (sameState(a, b) && sameEvents(a.out, b.out)) {
				commit(own);
				return;
			}
		}
		placeTag();
		commit(tagToks);
	}

	Tok commandTok(const StepParts &p, const Value &step, std::string src) {
		Tok t = tok(Tok::Command);
		t.step = step;
		t.attrs = worldAttrs(p);
		t.entities = p.entities;
		t.flag = p.cond.has_value();
		if (p.mp != nullptr) {
			std::string problem;
			src = withMacro(src, p.mp, &problem);
			if (!problem.empty()) note(problem);
		}
		t.src = src;
		return t;
	}

	// Writes the target step at idx; returns how many target steps it wrote
	size_t emitStep(size_t idx) {
		const Event &e = target[idx];
		const Value &step = e.step;
		StepParts p = splitStep(step);
		if (!p.ok && p.tag.empty()) {
			note(p.problem + ": " + shortJson(step));
			comment("scn2ks: " + shortJson(step));
			return 1;
		}
		if (!p.ok) note(p.problem);
		const std::string &tag = p.tag;
		int mode = model.s.mode;
		bool plain = p.flags.empty() && p.attrs.empty() && !p.cond && p.mp == nullptr;

		// steps the line modes make
		if (tag == "r" && plain) {
			const Event *n = peek(idx + 1);
			bool implied = n != nullptr && n->isText && !(n->flag & F_CONT) && !n->name && !n->disp &&
						   (mode == M_LINE || mode == M_VN || mode == M_FREE) && model.s.emptyLine && !model.s.afterPage && !model.s.afterName;
			if (implied) return 1; // the next text's start makes it
			placeTag();
			commit({ tok(Tok::R) });
			return 1;
		}
		if (stepIs(&e, { "l", nullptr, "voicewait", "true" })) {
			size_t n = stepIs(peek(idx + 1), { "afterline" }) ? 2 : 1;
			if ((mode == M_LINE || mode == M_VN) && !model.s.emptyLine && !(model.msg.open && model.msg.visible)) {
				commit({ tok(Tok::Eol) });
				return n;
			}
			note("an [l] that line mode " + std::string(modeName(mode)) + " doesn't make here");
			placeTag();
			Tok l = tok(Tok::L);
			l.src = "[l voicewait=true]";
			l.step = step;
			commit({ l });
			return 1;
		}
		bool noerFirst = tag == "*noer" && plain;
		const Event &pe = noerFirst && peek(idx + 1) ? target[idx + 1] : e;
		if (noerFirst && !(stepIs(peek(idx + 1), { "p", nullptr, "nosave", "true" }) ||
						   (peek(idx + 1) && !peek(idx + 1)->isText && splitStep(peek(idx + 1)->step).tag == "p"))) {
			note("a [noer] without the [p] after it");
			placeTag();
			commit({ tok(Tok::Noer) });
			return 1;
		}
		if (stepIs(&pe, { "p", nullptr, "nosave", "true" }) && mode != M_NONE) {
			std::vector<Tok> pre;
			if (noerFirst) pre.push_back(tok(Tok::Noer));
			std::vector<std::vector<Tok>> cands;
			auto with = [&](std::vector<Tok> x) {
				std::vector<Tok> r = pre;
				r.insert(r.end(), x.begin(), x.end());
				return r;
			};
			// the line's end, or an empty line, where the line mode makes the [p]; else [p]
			if (!sink.cur.empty()) cands.push_back(with({ tok(Tok::Eol) }));
			if (!sink.cur.empty()) cands.push_back(with({ tok(Tok::Eol), tok(Tok::Blank) }));
			else cands.push_back(with({ tok(Tok::Blank) }));
			size_t need = noerFirst ? 2 : 1;
			for (const auto &c : cands) {
				if (fits(c, nullptr, need)) {
					commit(c);
					return need;
				}
			}
			if (model.s.emptyLine) placeTag();
			commit(with({ tok(Tok::P), tok(Tok::Eol) }));
			return need;
		}
		if (noerFirst || (tag == "p" && !p.cond && mode == M_NONE) || (tag == "l" && !p.cond)) {
			// [l]/[p] written as tags (line mode none, or [l] with no text before it)
			std::vector<Tok> pre;
			const Value &ps = noerFirst ? target[idx + 1].step : step;
			StepParts pp = noerFirst ? splitStep(ps) : p;
			if (noerFirst) pre.push_back(tok(Tok::Noer));
			Tok t = tok(pp.tag == "l" ? Tok::L : Tok::P);
			t.src = tagSource(pp, pp.tag);
			t.step = ps;
			placeTag();
			pre.push_back(t);
			if (!fits(pre)) {
				if (pp.tag == "p" && mode != M_NONE) note("a [p] with attributes, which the line modes drop");
			}
			commit(pre);
			return noerFirst ? 2 : 1;
		}

		// text tags with cond= are steps of their own
		if ((tag == "ch" || tag == "r" || tag == "l" || tag == "p" || tag == "ruby") && p.cond) {
			placeTag();
			Tok t = tok(Tok::Early);
			t.step = step;
			t.src = tagSource(p, tag);
			if (p.mp) t.src = withMacro(t.src, p.mp, nullptr);
			commit({ t });
			return 1;
		}

		// [if] and the like
		if (tag == "*if" || tag == "*elsif" || tag == "*else" || tag == "*endif") {
			std::string name = tag.substr(1);
			Tok t = tok(Tok::Control, name);
			t.step = step;
			OStr exp = attrOf(p.attrs, "exp");
			t.src = "[" + name + (exp ? " exp=" + attrValue(*exp, false) : std::string()) + "]";
			if (p.mp) t.src = withMacro(t.src, p.mp, nullptr);
			if ((name == "elsif" || name == "else" || name == "endif") && model.ifs.empty()) {
				note("[" + name + "] without [if]");
				comment("scn2ks: " + t.src);
				return 1;
			}
			commitTag({ t });
			return 1;
		}
		if (tag == "*script") {
			Tok t = tok(Tok::Script, attrOf(p.attrs, "script").value_or(""));
			t.step = step;
			breakLine();
			commit({ t });
			return 1;
		}
		if (tag == "*emb") {
			Tok t = tok(Tok::Early);
			t.step = step;
			t.src = tagSource(p, "emb");
			if (p.mp) t.src = withMacro(t.src, p.mp, nullptr);
			commitTag({ t });
			return 1;
		}
		if (tag == "*noer") {
			placeTag();
			commit({ tok(Tok::Noer) });
			note("a [noer] that doesn't come out as written");
			return 1;
		}
		if (tag == "next") {
			// a jump that's a step: with cond=, inside [if], or to where an entity says
			bool entity = isEntity(p, "storage") || isEntity(p, "target");
			placeTag();
			if (entity || p.cond || !model.ifs.empty()) {
				Tok t = tok(Tok::Early);
				t.step = step;
				t.flag = !p.cond;
				if (!entity) {
					for (const auto &a : p.attrs) {
						if (a.first != "storage" && a.first != "target") note("a jump's attribute " + a.first + " (left out)");
					}
				}
				t.src = tagSource(p, "jump");
				if (p.mp) t.src = withMacro(t.src, p.mp, nullptr);
				commit({ t });
				return 1;
			}
			commit({ commandTok(p, step, tagSource(p, "next")) });
			return 1;
		}

		// tags the compiler takes for something else
		static const char *taken[] = { "if", "ignore", "elsif", "else", "endif", "endignore", "jump", "call", "return",
									   "emb", "ch", "r", "l", "p", "ruby", "nor", "noer", "nul", "voice", "nextvoice",
									   "parse", "seladd", "mseladd", "select", "mselect", "scenestart", "scenestarttext",
									   "sceneplay", "scenecheck", "macro", "endmacro", "erasemacro", "macropop", "pmacro",
									   "erasepmacro", "iscript", "endscript" };
		bool isTaken = false;
		for (const char *x : taken) isTaken = isTaken || tag == x;
		if (isTaken || tag.empty() || (tag[0] == '*' && tag.size() > 1) || tag.find(',') != std::string::npos ||
			tag.find('\'') != std::string::npos || hasAsciiUpper(tag)) {
			note("the step " + shortJson(step) + " can't be written as a tag (left as a comment)");
			comment("scn2ks: " + tagSource(p, tag));
			return 1;
		}
		for (const auto &f : p.flags) {
			if (!validAttrName(f)) note("the step " + tag + " has an attribute that can't be written: " + f);
		}
		for (const auto &a : p.attrs) {
			if (!validAttrName(a.first)) note("the step " + tag + " has an attribute that can't be written: " + a.first);
			if (a.second.find('\n') != std::string::npos) note("the step " + tag + " has a value with a new line in it");
		}
		if (p.mp != nullptr) {
			// the steps after it that came from the same macro call go in the same macro
			std::vector<Tok> toks;
			std::string body = tagSource(p, tag);
			Tok first = commandTok(p, step, "");
			first.src.clear();
			toks.push_back(first);
			size_t j = idx + 1;
			for (; j < target.size(); j++) {
				const Event &e2 = target[j];
				if (e2.isText) break;
				StepParts p2 = splitStep(e2.step);
				if (!p2.ok || p2.mp == nullptr || !scnpsb::equal(*p2.mp, *p.mp) || !plainTag(p2.tag)) break;
				body += tagSource(p2, p2.tag);
				Tok t = commandTok(p2, e2.step, "");
				t.src.clear();
				toks.push_back(t);
			}
			std::string problem;
			toks[0].src = withMacro(body, p.mp, &problem);
			if (!problem.empty()) note(problem);
			commitTag(toks);
			return j - idx;
		}
		commitTag({ commandTok(p, step, tagSource(p, tag)) });
		return 1;
	}

	// Whether a step is a tag the compiler keeps as it is (not text, a wait, [if] or a jump)
	static bool plainTag(const std::string &tag) {
		static const char *special[] = { "if", "ignore", "elsif", "else", "endif", "endignore", "jump", "call", "return",
										 "emb", "ch", "r", "l", "p", "ruby", "nor", "noer", "nul", "voice", "nextvoice",
										 "parse", "seladd", "mseladd", "select", "mselect", "scenestart", "scenestarttext",
										 "sceneplay", "scenecheck", "macro", "endmacro", "erasemacro", "macropop", "pmacro",
										 "erasepmacro", "iscript", "endscript", "next" };
		if (tag.empty() || tag[0] == '*' || hasAsciiUpper(tag)) return false;
		for (const char *x : special) if (tag == x) return false;
		return true;
	}

	// --- where a scene goes next

	static std::string stripParens(const std::string &e) {
		if (e.size() < 2 || e.front() != '(' || e.back() != ')') return e;
		int depth = 0;
		for (size_t i = 0; i < e.size(); i++) {
			if (e[i] == '(') depth++;
			else if (e[i] == ')') {
				depth--;
				if (depth == 0 && i + 1 < e.size()) return e; // (a) && (b)
			}
		}
		return e.substr(1, e.size() - 2);
	}

	void emitNexts(const TScene &sc, const std::string &nextLabel, bool last) {
		std::vector<const Value *> nexts;
		if (isList(sc.nexts)) for (const Value &n : *sc.nexts->list) nexts.push_back(&n);
		if (nexts.empty()) {
			if (!last && !model.dead) {
				// the story stops: a [select] without choices ends the scene with nowhere to go
				placeTag();
				Tok t = tok(Tok::Select);
				t.src = "[select]";
				commit({ t });
				sink.cur += " ; the story stops here";
			}
			return;
		}
		if (nexts.size() == 1 && intOf(member(nexts[0], "type")) == 0) {
			const Value *n = nexts[0];
			std::string target = isString(member(n, "target")) ? member(n, "target")->s : "";
			bool hasStorage = !isNull(member(n, "storage"));
			if (!isNull(member(n, "exp")) || !isNull(member(n, "eval"))) note("where the scene goes has exp= or eval=, which [jump] can't say");
			if (!hasStorage && !last && target == nextLabel) return; // it runs on into the next scene
			placeTag();
			std::string src = "[jump";
			if (hasStorage) src += " storage=" + attrValue(scalarText(*member(n, "storage")), false);
			if (!target.empty()) src += " target=" + attrValue(target, false);
			Tok t = tok(Tok::JumpEnd);
			t.src = src + "]";
			commit({ t });
			return;
		}
		// choices
		int type = -100;
		for (const Value *n : nexts) {
			int ty = int(intOf(member(n, "type")));
			if (ty <= 0) {
				note("where the scene goes mixes choices with a jump or an exit: " + shortJson(*n));
				comment("scn2ks: " + shortJson(*n));
				continue;
			}
			if (type != -100 && ty != type) {
				Tok s = tok(Tok::Select);
				s.src = type == 2 ? "[mselect]" : "[select]";
				placeTag();
				commit({ s });
				note("choices of both kinds");
			}
			type = ty;
			if (n->type != VT::Object) continue;
			std::string src = ty == 2 ? "[mseladd" : "[seladd";
			std::vector<std::pair<std::string, Value>> members(n->object->begin(), n->object->end());
			static const char *order[] = { "text", "storage", "target", "exp", "eval", "onceflag", "tag" };
			auto rank = [](const std::string &k) {
				for (size_t i = 0; i < sizeof order / sizeof order[0]; i++) if (k == order[i]) return int(i);
				return 100;
			};
			std::sort(members.begin(), members.end(), [&](const auto &a, const auto &b) {
				return rank(a.first) != rank(b.first) ? rank(a.first) < rank(b.first) : a.first < b.first;
			});
			for (const auto &m : members) {
				if (m.first == "type") continue;
				std::string v = scalarText(m.second);
				if (m.first == "eval") v = stripParens(v);
				if (!validAttrName(m.first)) {
					note("a choice's attribute " + m.first + " can't be written");
					continue;
				}
				src += " " + m.first + "=" + attrValue(v, false);
			}
			placeTag();
			Tok t = tok(Tok::Seladd);
			t.src = src + "]";
			commit({ t });
		}
		if (type != -100) {
			placeTag();
			Tok s = tok(Tok::Select);
			s.src = type == 2 ? "[mselect]" : "[select]";
			commit({ s });
		}
	}
};

// ------------------------------------------------------------------------
// Reading the file

Event textEvent(const Value &t, int id, std::vector<std::string> &problems) {
	Event e;
	e.isText = true;
	e.id = id;
	if (t.type != VT::List) {
		problems.push_back("text " + std::to_string(id) + " isn't a list: " + shortJson(t));
		return e;
	}
	const auto &l = *t.list;
	auto at = [&](size_t i) -> const Value * { return i < l.size() ? &l[i] : nullptr; };
	if (isString(at(0))) e.name = at(0)->s;
	const Value *disp = at(1);
	const Value *text = at(2);
	const Value *voice = at(3);
	const Value *flag = at(4);
	if (isString(disp)) e.disp = disp->s;
	else if (!isNull(disp)) problems.push_back("text " + std::to_string(id) + "'s display name isn't a string: " + shortJson(*disp));
	if (isString(text)) e.text = text->s;
	else if (!isNull(text)) problems.push_back("text " + std::to_string(id) + "'s text isn't a string: " + shortJson(*text));
	if (isString(voice) || isInt(voice)) {
		VEntry v;
		v.name = e.name.value_or("");
		v.voice = vvalOf(voice);
		e.voice.push_back(v);
	} else if (isList(voice)) {
		for (const Value &x : *voice->list) {
			VEntry v;
			const Value *n = member(&x, "name");
			v.name = isString(n) ? n->s : "";
			v.voice = vvalOf(member(&x, "voice"));
			v.noplayvoice = vvalOf(member(&x, "noplayvoice"));
			e.voice.push_back(v);
		}
	}
	e.flag = int(intOf(flag));
	return e;
}

std::string guessMode(const std::vector<TScene> &scenes) {
	bool nonCont = false, lineFlag = false, contNl = false;
	for (const TScene &sc : scenes) {
		for (size_t i = 0; i < sc.target.size(); i++) {
			const Event &e = sc.target[i];
			if (!e.isText) {
				if (e.step.type == VT::List && !e.step.list->empty() && (*e.step.list)[0].type == VT::String &&
					(*e.step.list)[0].s == "linemode")
					goto done;
				continue;
			}
			if (!(e.flag & F_CONT)) nonCont = true;
			if (e.flag & F_LINE) lineFlag = true;
			if ((e.flag & F_CONT) && !e.text.empty() && e.text[0] == '\n') contNl = true;
		}
	}
done:
	if (contNl) return "vn";
	if (nonCont) return lineFlag ? "line" : "page";
	return "none";
}

// ------------------------------------------------------------------------
// Reading JSON

class JsonReader {
public:
	explicit JsonReader(const std::string &t) : s(t) {}

	Value document() {
		Value v = value(0);
		space();
		if (p != s.size()) fail("text after the JSON");
		return v;
	}

private:
	const std::string &s;
	size_t p = 0;

	[[noreturn]] void fail(const std::string &what) {
		size_t line = 1, col = 1;
		for (size_t i = 0; i < p && i < s.size(); i++) {
			if (s[i] == '\n') {
				line++;
				col = 1;
			} else {
				col++;
			}
		}
		throw Error("not JSON (line " + std::to_string(line) + ", column " + std::to_string(col) + "): " + what);
	}

	void space() {
		while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\n' || s[p] == '\r')) p++;
	}

	bool word(const char *w) {
		size_t n = std::strlen(w);
		if (s.compare(p, n, w) != 0) return false;
		p += n;
		return true;
	}

	static void utf8(std::string &out, uint32_t c) {
		if (c < 0x80) {
			out += char(c);
		} else if (c < 0x800) {
			out += char(0xC0 | (c >> 6));
			out += char(0x80 | (c & 0x3F));
		} else if (c < 0x10000) {
			out += char(0xE0 | (c >> 12));
			out += char(0x80 | ((c >> 6) & 0x3F));
			out += char(0x80 | (c & 0x3F));
		} else {
			out += char(0xF0 | (c >> 18));
			out += char(0x80 | ((c >> 12) & 0x3F));
			out += char(0x80 | ((c >> 6) & 0x3F));
			out += char(0x80 | (c & 0x3F));
		}
	}

	uint32_t hex4() {
		if (p + 4 > s.size()) fail("a \\u escape cut short");
		uint32_t v = 0;
		for (int i = 0; i < 4; i++) {
			char c = s[p++];
			v <<= 4;
			if (c >= '0' && c <= '9') v |= uint32_t(c - '0');
			else if (c >= 'a' && c <= 'f') v |= uint32_t(c - 'a' + 10);
			else if (c >= 'A' && c <= 'F') v |= uint32_t(c - 'A' + 10);
			else fail("a \\u escape that isn't hexadecimal");
		}
		return v;
	}

	std::string string() {
		p++; // the "
		std::string out;
		while (true) {
			if (p >= s.size()) fail("a string without its closing \"");
			char c = s[p++];
			if (c == '"') return out;
			if (c != '\\') {
				out += c;
				continue;
			}
			if (p >= s.size()) fail("an escape cut short");
			char e = s[p++];
			switch (e) {
			case '"': out += '"'; break;
			case '\\': out += '\\'; break;
			case '/': out += '/'; break;
			case 'b': out += '\b'; break;
			case 'f': out += '\f'; break;
			case 'n': out += '\n'; break;
			case 'r': out += '\r'; break;
			case 't': out += '\t'; break;
			case 'u': {
				uint32_t c1 = hex4();
				if (c1 >= 0xD800 && c1 < 0xDC00 && s.compare(p, 2, "\\u") == 0) {
					size_t back = p;
					p += 2;
					uint32_t c2 = hex4();
					if (c2 >= 0xDC00 && c2 < 0xE000) {
						utf8(out, 0x10000 + ((c1 - 0xD800) << 10) + (c2 - 0xDC00));
						break;
					}
					p = back;
				}
				utf8(out, (c1 >= 0xD800 && c1 < 0xE000) ? 0xFFFD : c1);
				break;
			}
			default: fail(std::string("the escape \\") + e);
			}
		}
	}

	Value number() {
		size_t start = p;
		bool real = false;
		if (s[p] == '-') p++;
		while (p < s.size() && ((s[p] >= '0' && s[p] <= '9') || s[p] == '.' || s[p] == 'e' || s[p] == 'E' ||
								s[p] == '+' || s[p] == '-')) {
			if (s[p] == '.' || s[p] == 'e' || s[p] == 'E') real = true;
			p++;
		}
		std::string t = s.substr(start, p - start);
		char *end = nullptr;
		if (!real) {
			errno = 0;
			long long v = std::strtoll(t.c_str(), &end, 10);
			if (*end == '\0' && errno == 0 && !t.empty() && t != "-") return Value::makeInt(v);
		}
		double d = std::strtod(t.c_str(), &end);
		if (t.empty() || *end != '\0') fail("a number that isn't one");
		return Value::makeReal(d);
	}

	Value value(int depth) {
		if (depth > 1000) fail("nested over 1000 deep");
		space();
		if (p >= s.size()) fail("the JSON ends early");
		char c = s[p];
		if (c == '{') {
			p++;
			Value o = Value::makeObject();
			space();
			if (p < s.size() && s[p] == '}') {
				p++;
				return o;
			}
			while (true) {
				space();
				if (p >= s.size() || s[p] != '"') fail("an object's member without a name");
				std::string name = string();
				space();
				if (p >= s.size() || s[p] != ':') fail("a member's name without :");
				p++;
				Value v = value(depth + 1);
				bool replaced = false;
				for (auto &m : *o.object) {
					if (m.first == name) {
						m.second = v;
						replaced = true;
					}
				}
				if (!replaced) o.object->push_back({ name, v });
				space();
				if (p < s.size() && s[p] == ',') {
					p++;
					continue;
				}
				if (p < s.size() && s[p] == '}') {
					p++;
					return o;
				}
				fail("an object without its }");
			}
		}
		if (c == '[') {
			p++;
			Value l = Value::makeList();
			space();
			if (p < s.size() && s[p] == ']') {
				p++;
				return l;
			}
			while (true) {
				l.list->push_back(value(depth + 1));
				space();
				if (p < s.size() && s[p] == ',') {
					p++;
					continue;
				}
				if (p < s.size() && s[p] == ']') {
					p++;
					return l;
				}
				fail("a list without its ]");
			}
		}
		if (c == '"') return Value::makeString(string());
		if (c == '-' || (c >= '0' && c <= '9')) return number();
		if (word("null") || word("void")) return Value();
		if (word("true")) return Value::makeBool(true);
		if (word("false")) return Value::makeBool(false);
		fail(std::string("unexpected ") + c);
	}
};

// UTF-16 (with its BOM) as UTF-8
std::string fromUtf16(const std::string &t, bool bigEndian) {
	std::string out;
	auto unit = [&](size_t i) {
		uint8_t a = uint8_t(t[i]), b = uint8_t(t[i + 1]);
		return bigEndian ? uint32_t(a << 8 | b) : uint32_t(b << 8 | a);
	};
	for (size_t i = 2; i + 1 < t.size(); i += 2) {
		uint32_t c = unit(i);
		if (c >= 0xD800 && c < 0xDC00 && i + 3 < t.size()) {
			uint32_t d = unit(i + 2);
			if (d >= 0xDC00 && d < 0xE000) {
				c = 0x10000 + ((c - 0xD800) << 10) + (d - 0xDC00);
				i += 2;
			}
		}
		if (c >= 0xD800 && c < 0xE000) c = 0xFFFD;
		if (c < 0x80) out += char(c);
		else if (c < 0x800) { out += char(0xC0 | (c >> 6)); out += char(0x80 | (c & 0x3F)); }
		else if (c < 0x10000) { out += char(0xE0 | (c >> 12)); out += char(0x80 | ((c >> 6) & 0x3F)); out += char(0x80 | (c & 0x3F)); }
		else { out += char(0xF0 | (c >> 18)); out += char(0x80 | ((c >> 12) & 0x3F)); out += char(0x80 | ((c >> 6) & 0x3F)); out += char(0x80 | (c & 0x3F)); }
	}
	return out;
}

} // namespace

Value fromJson(const std::string &text) {
	if (text.size() >= 2 && uint8_t(text[0]) == 0xFF && uint8_t(text[1]) == 0xFE) return JsonReader(fromUtf16(text, false)).document();
	if (text.size() >= 2 && uint8_t(text[0]) == 0xFE && uint8_t(text[1]) == 0xFF) return JsonReader(fromUtf16(text, true)).document();
	if (text.size() >= 3 && text.compare(0, 3, "\xEF\xBB\xBF") == 0) return JsonReader(text.substr(3)).document();
	return JsonReader(text).document();
}

std::string toJson(const Value &value) {
	std::string out;
	json(out, value, 0);
	out += "\n";
	return out;
}

Result decompile(const Value &root, const Options &opt) {
	Result res;
	const Value *scenesV = member(&root, "scenes");
	if (!isList(scenesV)) throw Error("not a compiled scenario: it has no scenes");

	// the scenes, and what each holds
	std::vector<TScene> scenes;
	std::vector<std::string> fileProblems;
	for (const Value &s : *scenesV->list) {
		if (s.type != VT::Object) {
			fileProblems.push_back("scene " + std::to_string(scenes.size() + 1) + " isn't a scene (left out): " + shortJson(s));
			continue;
		}
		TScene sc;
		sc.raw = &s;
		const Value *label = member(&s, "label");
		const Value *title = member(&s, "title");
		sc.label = isString(label) ? label->s : "";
		sc.title = isString(title) ? title->s : "";
		sc.texts = member(&s, "texts");
		sc.nexts = member(&s, "nexts");
		const Value *lines = member(&s, "lines");
		std::string where = sc.label.empty() ? "the start" : "scene " + sc.label;
		if (isList(lines)) {
			for (const Value &item : *lines->list) {
				if (item.type == VT::String || isSavePoint(item)) continue; // read flags, save points
				if (isInt(&item)) {
					int id = int(intOf(&item));
					std::vector<std::string> problems;
					Event e;
					if (isList(sc.texts) && id >= 1 && size_t(id) <= sc.texts->list->size()) {
						e = textEvent((*sc.texts->list)[size_t(id - 1)], id, problems);
					} else {
						problems.push_back("text " + std::to_string(id) + " isn't in the scene");
						e.isText = true;
						e.id = id;
					}
					for (const auto &p : problems) fileProblems.push_back(where + ": " + p);
					sc.target.push_back(e);
				} else {
					Event e;
					e.step = item;
					sc.target.push_back(e);
				}
			}
		}
		for (const auto &m : *s.object) {
			static const char *known[] = { "label", "title", "firstLine", "startLine", "lastLine", "spCount", "texts",
										   "lines", "nexts" };
			bool k = false;
			for (const char *x : known) k = k || m.first == x;
			if (!k) fileProblems.push_back(where + ": the scene's " + m.first + " can't be written (" + shortJson(m.second) + ")");
		}
		scenes.push_back(std::move(sc));
	}

	std::string start = opt.lineMode == "auto" ? guessMode(scenes) : opt.lineMode;
	res.startMode = start;

	std::vector<std::string> notes;
	Emitter em(opt, notes);
	em.model.globalIgnoreCR = opt.ignoreCR;
	em.model.setLineMode(start);
	em.model.setCrAfterName(opt.crAfterName ? OStr("true") : std::nullopt);

	std::vector<std::string> out;
	{
		std::string head = "; Written by scn2ks";
		if (!opt.source.empty()) head += " from " + opt.source;
		const Value *storage = member(&root, "storage");
		if (isString(storage)) head += " (compiled from " + storage->s + ")";
		out.push_back(head);
		out.push_back(std::string("; Line mode at the start: ") + start +
					  (start == "none" ? "" : " (as parseroption.txt's linemode=" + start + ")"));
		for (const auto &m : *root.object) {
			if (m.first != "scenes" && m.first != "version" && m.first != "storage" && m.first != "hash")
				fileProblems.push_back("the file's " + m.first + " isn't written (" + shortJson(m.second) + ")");
		}
	}

	std::map<std::string, int> labelCounts;
	std::string prevTitle;
	for (size_t si = 0; si < scenes.size(); si++) {
		TScene &sc = scenes[si];
		em.where = sc.label.empty() ? "the start" : sc.label;
		em.sceneNotes.clear();
		em.target = sc.target;
		em.produced = 0;
		size_t sceneStart = em.sink.lines.size();

		if (!sc.label.empty() || si > 0) {
			em.breakLine();
			// a label written twice is name:2, name:3... (KAGParser)
			std::string label = sc.label;
			size_t colon = label.rfind(':');
			if (colon != std::string::npos && colon > 1) {
				std::string base = label.substr(0, colon);
				int n = std::atoi(label.c_str() + colon + 1);
				if (n >= 2 && labelCounts[base] == n - 1) label = base;
			}
			labelCounts[label]++;
			if (label.empty() || label[0] != '*') {
				em.note("the label " + label + " doesn't start with *");
				label = "*" + label;
			}
			std::string line = label;
			if (sc.title != prevTitle) {
				if (sc.title.empty()) em.note("an empty title after one that isn't: it keeps the one before");
				line += "|" + sc.title;
			}
			if (label.find('|') != std::string::npos) em.note("the label " + label + " has a | in it");
			em.model.label();
			em.check(em.model.out.size());
			em.sink.cur = line;
			em.sink.newline();
			prevTitle = sc.title.empty() ? prevTitle : sc.title;
		}
		sceneStart = em.sink.lines.size();

		size_t idx = 0;
		while (idx < em.target.size()) {
			// what's still pending from the last text closes as the next thing is written
			const Event &e = em.target[idx];
			if (e.isText) {
				em.emitText(e, em.peek(idx + 1));
				idx++;
			} else {
				idx += em.emitStep(idx);
			}
		}
		std::string nextLabel = si + 1 < scenes.size() ? scenes[si + 1].label : "";
		em.emitNexts(sc, nextLabel, si + 1 == scenes.size());
		// what's still open closes where the scene ends
		{
			Model m = em.model;
			size_t before = m.out.size();
			m.label();
			if (m.out.size() > before) {
				// check it now (the label of the next scene does the same)
				Emitter &E = em;
				size_t b = E.model.out.size();
				E.model.closeMessage(F_NOWAIT);
				E.check(b);
			}
		}
		if (em.produced < em.target.size()) {
			em.note(std::to_string(em.target.size() - em.produced) + " of its texts and steps don't come out as they are");
		}
		res.scenes++;
		if (!em.sceneNotes.empty()) {
			std::vector<std::string> c;
			for (const auto &n : em.sceneNotes) {
				c.push_back("; scn2ks: " + n);
				res.notes.push_back(n);
			}
			em.sink.lines.insert(em.sink.lines.begin() + long(sceneStart), c.begin(), c.end());
		}
	}
	em.breakLine();
	for (const auto &p : fileProblems) res.notes.insert(res.notes.begin(), p);
	res.texts = em.textCount;

	std::string ks = opt.bom ? "\xEF\xBB\xBF" : "";
	for (const auto &l : out) ks += l + "\n";
	for (const auto &p : fileProblems) ks += "; scn2ks: " + p + "\n";
	for (const auto &l : em.sink.lines) ks += l + "\n";
	res.ks = ks;
	return res;
}

} // namespace scnks
