// main.cpp - the scnfile plugin: the SCNFile class for TJS
//
//   new SCNFile(storageOrOctet)   reads a PSB file (a storage name, or its bytes as an octet)
//   .root                         its contents, as Dictionaries, Arrays and plain values
//   SCNFile.save(value, storage)  writes a value as a plain PSB file
//   SCNFile.toOctet(value)        the same, as an octet
//
// Values map to PSB and back as: void <-> null, Integer <-> integer, Real <-> real, String <->
// string (UTF-8 in the file), Octet <-> resource, Array <-> list, Dictionary <-> object. A PSB
// boolean reads as the Integer 1 or 0. Other objects (functions, class instances) can't be
// written.
//
// SPDX-License-Identifier: 0BSD

#include "ncbind.hpp"
#include "psb.hpp"

#include <string>
#include <vector>

namespace {

// ------------------------------------------------------------------------
// Strings: TJS's are UTF-16, PSB's UTF-8
// ------------------------------------------------------------------------

std::string toUtf8(const tjs_char *s, size_t len) {
	std::string out;
	out.reserve(len);
	for (size_t k = 0; k < len; k++) {
		uint32_t c = uint16_t(s[k]);
		if (c >= 0xD800 && c < 0xDC00 && k + 1 < len && uint16_t(s[k + 1]) >= 0xDC00 && uint16_t(s[k + 1]) < 0xE000) {
			c = 0x10000 + ((c - 0xD800) << 10) + (uint16_t(s[k + 1]) - 0xDC00);
			k++;
		} else if (c >= 0xD800 && c < 0xE000) {
			c = 0xFFFD; // half a surrogate pair isn't a character
		}
		if (c < 0x80) {
			out.push_back(char(c));
		} else if (c < 0x800) {
			out.push_back(char(0xC0 | (c >> 6)));
			out.push_back(char(0x80 | (c & 0x3F)));
		} else if (c < 0x10000) {
			out.push_back(char(0xE0 | (c >> 12)));
			out.push_back(char(0x80 | ((c >> 6) & 0x3F)));
			out.push_back(char(0x80 | (c & 0x3F)));
		} else {
			out.push_back(char(0xF0 | (c >> 18)));
			out.push_back(char(0x80 | ((c >> 12) & 0x3F)));
			out.push_back(char(0x80 | ((c >> 6) & 0x3F)));
			out.push_back(char(0x80 | (c & 0x3F)));
		}
	}
	return out;
}

std::string toUtf8(const ttstr &s) {
	return toUtf8(s.c_str(), size_t(s.GetLen()));
}

// Invalid UTF-8 comes out as U+FFFD
ttstr fromUtf8(const std::string &s) {
	static const uint32_t minimum[4] = { 0, 0x80, 0x800, 0x10000 }; // no overlong forms
	std::vector<tjs_char> out;
	out.reserve(s.size() + 1);
	const unsigned char *p = reinterpret_cast<const unsigned char *>(s.data());
	size_t n = s.size();
	for (size_t k = 0; k < n;) {
		uint32_t c = p[k];
		int extra = c < 0x80 ? 0 : (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : (c >> 3) == 30 ? 3 : -1;
		bool ok = extra >= 0 && k + size_t(extra) < n;
		if (ok) {
			c = extra == 0 ? c : extra == 1 ? (c & 0x1F) : extra == 2 ? (c & 0x0F) : (c & 0x07);
			for (int e = 1; e <= extra; e++) {
				if ((p[k + e] & 0xC0) != 0x80) {
					ok = false;
					break;
				}
				c = (c << 6) | (p[k + e] & 0x3F);
			}
		}
		if (!ok || c < minimum[extra < 0 ? 0 : extra] || c > 0x10FFFF || (c >= 0xD800 && c < 0xE000)) {
			out.push_back(tjs_char(0xFFFD));
			k++;
			continue;
		}
		k += size_t(extra) + 1;
		if (c >= 0x10000) {
			c -= 0x10000;
			out.push_back(tjs_char(0xD800 + (c >> 10)));
			out.push_back(tjs_char(0xDC00 + (c & 0x3FF)));
		} else {
			out.push_back(tjs_char(c));
		}
	}
	out.push_back(0);
	return ttstr(out.data());
}

// ------------------------------------------------------------------------
// TJS values <-> the PSB tree
// ------------------------------------------------------------------------

tTJSVariant toVariant(const scnpsb::Value &v) {
	switch (v.type) {
	case scnpsb::Value::Type::Null:
		return tTJSVariant();
	case scnpsb::Value::Type::Bool:
		return tTJSVariant(tjs_int(v.b ? 1 : 0));
	case scnpsb::Value::Type::Int:
		return tTJSVariant(tTVInteger(v.i));
	case scnpsb::Value::Type::Real:
		return tTJSVariant(tTVReal(v.d));
	case scnpsb::Value::Type::String:
		return tTJSVariant(fromUtf8(v.s));
	case scnpsb::Value::Type::Resource:
		return tTJSVariant(reinterpret_cast<const tjs_uint8 *>(v.s.data()), tjs_uint(v.s.size()));
	case scnpsb::Value::Type::List: {
		iTJSDispatch2 *array = TJSCreateArrayObject();
		tTJSVariant result(array, array);
		array->Release();
		if (v.list) {
			tjs_int k = 0;
			for (const scnpsb::Value &c : *v.list) {
				tTJSVariant item = toVariant(c);
				array->PropSetByNum(TJS_MEMBERENSURE, k++, &item, array);
			}
		}
		return result;
	}
	case scnpsb::Value::Type::Object: {
		iTJSDispatch2 *dict = TJSCreateDictionaryObject();
		tTJSVariant result(dict, dict);
		dict->Release();
		if (v.object) {
			for (const auto &m : *v.object) {
				tTJSVariant item = toVariant(m.second);
				ttstr name = fromUtf8(m.first);
				dict->PropSet(TJS_MEMBERENSURE, name.c_str(), NULL, &item, dict);
			}
		}
		return result;
	}
	}
	return tTJSVariant();
}

scnpsb::Value fromVariant(const tTJSVariant &v, int depth);

// Collects a Dictionary's members (EnumMembers calls it once per member)
class MemberCollector : public tTJSDispatch {
public:
	MemberCollector(scnpsb::Object &object, int depth) : object(object), depth(depth) {}

	tjs_error TJS_INTF_METHOD FuncCall(tjs_uint32 flag, const tjs_char *membername, tjs_uint32 *hint,
									   tTJSVariant *result, tjs_int numparams, tTJSVariant **param,
									   iTJSDispatch2 *objthis) {
		if (numparams >= 3) {
			tTVInteger flags = param[1]->AsInteger();
			if (!(flags & TJS_HIDDENMEMBER)) {
				object.emplace_back(toUtf8(param[0]->GetString()), fromVariant(*param[2], depth + 1));
			}
		}
		if (result) *result = (tjs_int)1;
		return TJS_S_OK;
	}

private:
	scnpsb::Object &object;
	int depth;
};

bool isInstanceOf(iTJSDispatch2 *obj, const tjs_char *name) {
	return obj->IsInstanceOf(0, NULL, NULL, name, obj) == TJS_S_TRUE;
}

scnpsb::Value fromVariant(const tTJSVariant &v, int depth) {
	if (depth > 1000) TVPThrowExceptionMessage(TJS_W("SCNFile: values nested over 1000 deep (does an object hold itself?)"));
	switch (v.Type()) {
	case tvtVoid:
		return scnpsb::Value();
	case tvtInteger:
		return scnpsb::Value::makeInt(int64_t(v.AsInteger()));
	case tvtReal:
		return scnpsb::Value::makeReal(double(v.AsReal()));
	case tvtString:
		return scnpsb::Value::makeString(toUtf8(ttstr(v.AsStringNoAddRef())));
	case tvtOctet: {
		tTJSVariantOctet *o = v.AsOctetNoAddRef();
		if (!o) return scnpsb::Value::makeResource(std::string());
		return scnpsb::Value::makeResource(std::string(reinterpret_cast<const char *>(o->GetData()), o->GetLength()));
	}
	case tvtObject: {
		iTJSDispatch2 *obj = v.AsObjectNoAddRef();
		if (!obj) return scnpsb::Value();
		if (isInstanceOf(obj, TJS_W("Array"))) {
			tTJSVariant count;
			obj->PropGet(0, TJS_W("count"), NULL, &count, obj);
			scnpsb::Value list = scnpsb::Value::makeList();
			tjs_int n = tjs_int(count.AsInteger());
			list.list->reserve(size_t(n));
			for (tjs_int k = 0; k < n; k++) {
				tTJSVariant item;
				obj->PropGetByNum(TJS_IGNOREPROP, k, &item, obj);
				list.list->push_back(fromVariant(item, depth + 1));
			}
			return list;
		}
		if (isInstanceOf(obj, TJS_W("Dictionary"))) {
			scnpsb::Value object = scnpsb::Value::makeObject();
			MemberCollector *collector = new MemberCollector(*object.object, depth);
			tTJSVariantClosure closure(collector);
			obj->EnumMembers(TJS_IGNOREPROP, &closure, obj);
			collector->Release();
			return object;
		}
		TVPThrowExceptionMessage(TJS_W("SCNFile: only Arrays and Dictionaries can be written, not other objects"));
	}
	default:
		break;
	}
	TVPThrowExceptionMessage(TJS_W("SCNFile: a value of a kind that can't be written"));
	return scnpsb::Value();
}

std::vector<uint8_t> readStorage(const ttstr &name) {
	iTJSBinaryStream *in = TVPCreateStream(name, TJS_BS_READ);
	std::vector<uint8_t> bytes;
	try {
		tjs_uint64 size = in->GetSize();
		if (size > 0x7FFFFFFF) TVPThrowExceptionMessage(TJS_W("SCNFile: the file is too large"));
		bytes.resize(size_t(size));
		if (size > 0 && in->Read(bytes.data(), tjs_uint(size)) != tjs_uint(size)) {
			TVPThrowExceptionMessage(TJS_W("SCNFile: the file couldn't be read whole"));
		}
	} catch (...) {
		in->Destruct();
		throw;
	}
	in->Destruct();
	return bytes;
}

void writeStorage(const ttstr &name, const std::vector<uint8_t> &bytes) {
	iTJSBinaryStream *out = TVPCreateStream(name, TJS_BS_WRITE);
	try {
		if (!bytes.empty() && out->Write(bytes.data(), tjs_uint(bytes.size())) != tjs_uint(bytes.size())) {
			TVPThrowExceptionMessage(TJS_W("SCNFile: the file couldn't be written whole"));
		}
	} catch (...) {
		out->Destruct();
		throw;
	}
	out->Destruct();
}

// A codec error as a TJS exception
void rethrow(const std::exception &e) {
	ttstr msg = TJS_W("SCNFile: ");
	msg += fromUtf8(e.what());
	TVPThrowExceptionMessage(msg.c_str());
}

std::vector<uint8_t> encode(const tTJSVariant &value) {
	scnpsb::Value tree = fromVariant(value, 0);
	try {
		return scnpsb::write(tree);
	} catch (const scnpsb::Error &e) {
		rethrow(e);
	}
	return std::vector<uint8_t>();
}

} // namespace

class SCNFile {
public:
	explicit SCNFile(const std::vector<uint8_t> &bytes) {
		scnpsb::Value tree;
		try {
			tree = scnpsb::read(bytes.data(), bytes.size());
		} catch (const scnpsb::Error &e) {
			rethrow(e);
		}
		root = toVariant(tree);
	}

	tTJSVariant getRoot() const { return root; }

	// new SCNFile(storageOrOctet)
	static tjs_error TJS_INTF_METHOD factory(SCNFile **inst, tjs_int numparams, tTJSVariant **param,
											 iTJSDispatch2 *objthis) {
		if (numparams < 1) return TJS_E_BADPARAMCOUNT;
		if (param[0]->Type() == tvtOctet) {
			tTJSVariantOctet *o = param[0]->AsOctetNoAddRef();
			std::vector<uint8_t> bytes;
			if (o) bytes.assign(o->GetData(), o->GetData() + o->GetLength());
			*inst = new SCNFile(bytes);
		} else {
			*inst = new SCNFile(readStorage(param[0]->GetString()));
		}
		return TJS_S_OK;
	}

	// SCNFile.save(value, storage)
	static tjs_error TJS_INTF_METHOD save(tTJSVariant *result, tjs_int numparams, tTJSVariant **param,
										  iTJSDispatch2 *objthis) {
		if (numparams < 2) return TJS_E_BADPARAMCOUNT;
		writeStorage(param[1]->GetString(), encode(*param[0]));
		return TJS_S_OK;
	}

	// SCNFile.toOctet(value)
	static tjs_error TJS_INTF_METHOD toOctet(tTJSVariant *result, tjs_int numparams, tTJSVariant **param,
											 iTJSDispatch2 *objthis) {
		if (numparams < 1) return TJS_E_BADPARAMCOUNT;
		std::vector<uint8_t> bytes = encode(*param[0]);
		if (result) *result = tTJSVariant(bytes.empty() ? NULL : bytes.data(), tjs_uint(bytes.size()));
		return TJS_S_OK;
	}

private:
	tTJSVariant root;
};

NCB_REGISTER_CLASS(SCNFile) {
	Factory(&Class::factory);
	Property(TJS_W("root"), &Class::getRoot, (int)0);
	RawCallback(TJS_W("save"), &Class::save, TJS_STATICMEMBER);
	RawCallback(TJS_W("toOctet"), &Class::toOctet, TJS_STATICMEMBER);
}
