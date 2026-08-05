#include "prosto/interpreter.hpp"
#include "prosto/utils_decl.hpp"
#include <nlohmann/json.hpp>

#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <regex>
#include <memory>
#include <algorithm>
#include <numeric>
#include <thread>
#include <chrono>
#include <filesystem>
#include <sqlite3.h>

namespace prosto {

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

std::vector<std::string> splitCommandLine(const std::string& cmd) {
    std::vector<std::string> args;
    std::string cur;
    char quote = 0;
    bool escape = false;
    for (char c : cmd) {
        if (escape) {
            cur.push_back(c);
            escape = false;
            continue;
        }
        if (c == '\\') {
            escape = true;
            continue;
        }
        if (quote) {
            if (c == quote) {
                quote = 0;
            } else {
                cur.push_back(c);
            }
            continue;
        }
        if (c == '"' || c == '\'') {
            quote = c;
            continue;
        }
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (!cur.empty()) {
                args.push_back(cur);
                cur.clear();
            }
            continue;
        }
        cur.push_back(c);
    }
    if (!cur.empty()) args.push_back(cur);
    return args;
}

} // namespace

using namespace std;

void Interpreter::registerBuiltins() {
    globals_["PROJECT_NAME"] = Value("Prosto+");
    globals_["VERSION"] = Value("1.0.0");
    globals_["system"] = Value(platformName());
    globals_["now"] = Value(formatDate("%Y-%m-%d %H:%M:%S"));

    builtins_["true"] = Value(true);
    builtins_["True"] = Value(true);
    builtins_["false"] = Value(false);
    builtins_["False"] = Value(false);
    builtins_["None"] = Value();
    builtins_["null"] = Value();

    builtins_["print"] = Value::makeNativeFunction([](Interpreter&, vector<Value>& args) {
        Value kw = getKwargsDict(args);
        string end = "\n";
        if (kw.isDict()) {
            auto p = kw.dict->find(Value("end"));
            if (p) end = p->toStr();
        }
        for (size_t i = 0; i < args.size(); i++) {
            if (i) cout << " ";
            cout << args[i].toStr();
        }
        cout << end;
        return Value();
    }, "print");

    builtins_["input"] = Value::makeNativeFunction([](Interpreter&, vector<Value>& args) {
        if (!args.empty()) cout << args[0].toStr();
        cout.flush();
        string line;
        getline(cin, line);
        return Value(line);
    }, "input");

    builtins_["exit"] = Value::makeNativeFunction([](Interpreter&, vector<Value>&) {
        exit(0);
        return Value();
    }, "exit");

    builtins_["abs"] = Value::makeNativeFunction([](Interpreter&, vector<Value>& args) {
        if (args.empty()) return Value(0LL);
        if (args[0].type == VT::Int) return Value(llabs(args[0].i));
        return Value(fabs(args[0].toFloat()));
    }, "abs");

    builtins_["len"] = Value::makeNativeFunction([](Interpreter&, vector<Value>& args) {
        if (args.empty()) return Value(0LL);
        Value& v = args[0];
        if (v.isString()) return Value((long long)v.s.size());
        if (v.isList()) return Value((long long)v.list->size());
        if (v.isDict()) return Value((long long)v.dict->items.size());
        return Value(0LL);
    }, "len");

    auto seqOrArgs = [](vector<Value>& args) -> vector<Value> {
        if (args.size() == 1 && args[0].isList()) return *args[0].list;
        return args;
    };

    builtins_["min"] = Value::makeNativeFunction([seqOrArgs](Interpreter&, vector<Value>& args) {
        auto vals = seqOrArgs(args);
        if (vals.empty()) throw ProstoError{"ValueError", "min() arg is empty"};
        Value best = vals[0];
        for (auto& v : vals) if (lessValue(v, best)) best = v;
        return best;
    }, "min");

    builtins_["max"] = Value::makeNativeFunction([seqOrArgs](Interpreter&, vector<Value>& args) {
        auto vals = seqOrArgs(args);
        if (vals.empty()) throw ProstoError{"ValueError", "max() arg is empty"};
        Value best = vals[0];
        for (auto& v : vals) if (lessValue(best, v)) best = v;
        return best;
    }, "max");

    builtins_["sum"] = Value::makeNativeFunction([seqOrArgs](Interpreter&, vector<Value>& args) {
        auto vals = seqOrArgs(args);
        bool allInt = true;
        long long si = 0;
        double sf = 0.0;
        for (auto& v : vals) {
            if (v.type != VT::Int) allInt = false;
            si += v.toInt();
            sf += v.toFloat();
        }
        if (allInt) return Value(si);
        return Value(sf);
    }, "sum");

    builtins_["str"] = Value::makeNativeFunction([](Interpreter&, vector<Value>& args) {
        if (args.empty()) return Value("");
        return Value(args[0].toStr());
    }, "str");

    builtins_["int"] = Value::makeNativeFunction([](Interpreter&, vector<Value>& args) {
        if (args.empty()) return Value(0LL);
        if (args.size() >= 2 && args[0].isString()) {
            return Value((long long)stoll(args[0].s, nullptr, (int)args[1].toInt()));
        }
        return Value(args[0].toInt());
    }, "int");

    builtins_["float"] = Value::makeNativeFunction([](Interpreter&, vector<Value>& args) {
        if (args.empty()) return Value(0.0);
        return Value(args[0].toFloat());
    }, "float");

    builtins_["round"] = Value::makeNativeFunction([](Interpreter&, vector<Value>& args) {
        if (args.empty()) return Value(0LL);
        long long n = args.size() > 1 ? args[1].toInt() : 0;
        double p = pow(10.0, (double)n);
        double r = std::round(args[0].toFloat() * p) / p;
        if (n <= 0) return Value((long long)r);
        return Value(r);
    }, "round");

    builtins_["upper"] = Value::makeNativeFunction([](Interpreter&, vector<Value>& args) {
        if (args.empty()) return Value("");
        return Value(upperString(args[0].toStr()));
    }, "upper");

    builtins_["lower"] = Value::makeNativeFunction([](Interpreter&, vector<Value>& args) {
        if (args.empty()) return Value("");
        return Value(lowerString(args[0].toStr()));
    }, "lower");

    builtins_["split"] = Value::makeNativeFunction([](Interpreter&, vector<Value>& args) {
        if (args.empty()) return makeListFromVector({});
        string sep;
        if (args.size() > 1 && !args[1].isNull()) sep = args[1].toStr();
        auto parts = splitString(args[0].toStr(), sep);
        vector<Value> out;
        for (auto& p : parts) out.push_back(Value(p));
        return makeListFromVector(out);
    }, "split");

    builtins_["join"] = Value::makeNativeFunction([](Interpreter&, vector<Value>& args) {
        if (args.empty() || !args[0].isList()) return Value("");
        string sep = args.size() > 1 ? args[1].toStr() : " ";
        string out;
        for (size_t i = 0; i < args[0].list->size(); i++) {
            if (i) out += sep;
            out += (*args[0].list)[i].toStr();
        }
        return Value(out);
    }, "join");

    builtins_["replace"] = Value::makeNativeFunction([](Interpreter&, vector<Value>& args) {
        if (args.size() < 3) throw ProstoError{"TypeError", "replace(s,old,new) requires 3 arguments"};
        string s = args[0].toStr();
        string old = args[1].toStr();
        string neu = args[2].toStr();
        size_t pos = 0;
        while ((pos = s.find(old, pos)) != string::npos) {
            s.replace(pos, old.size(), neu);
            pos += neu.size();
        }
        return Value(s);
    }, "replace");

    builtins_["find"] = Value::makeNativeFunction([](Interpreter&, vector<Value>& args) {
        if (args.size() < 2) return Value(-1LL);
        auto pos = args[0].toStr().find(args[1].toStr());
        return Value(pos == string::npos ? -1LL : (long long)pos);
    }, "find");

    builtins_["slice"] = Value::makeNativeFunction([](Interpreter& in, vector<Value>& args) {
        if (args.size() < 3) throw ProstoError{"TypeError", "slice(x,start,end) requires 3 arguments"};
        return in.sliceValue(args[0], args[1], args[2], Value());
    }, "slice");

    builtins_["time"] = Value::makeNativeFunction([](Interpreter&, vector<Value>&) {
        return Value((long long)chrono::duration_cast<chrono::seconds>(
            chrono::system_clock::now().time_since_epoch()).count());
    }, "time");

    builtins_["date"] = Value::makeNativeFunction([](Interpreter&, vector<Value>& args) {
        string fmt = args.empty() ? "%Y-%m-%d %H:%M:%S" : args[0].toStr();
        return Value(formatDate(fmt));
    }, "date");

    builtins_["basename"] = Value::makeNativeFunction([](Interpreter&, vector<Value>& args) {
        if (args.empty()) return Value("");
        return Value(fs::path(args[0].toStr()).filename().string());
    }, "basename");

    builtins_["dirname"] = Value::makeNativeFunction([](Interpreter&, vector<Value>& args) {
        if (args.empty()) return Value("");
        return Value(fs::path(args[0].toStr()).parent_path().string());
    }, "dirname");

    builtins_["extname"] = Value::makeNativeFunction([](Interpreter&, vector<Value>& args) {
        if (args.empty()) return Value("");
        return Value(fs::path(args[0].toStr()).extension().string());
    }, "extname");

    builtins_["joinpath"] = Value::makeNativeFunction([](Interpreter&, vector<Value>& args) {
        if (args.size() < 2) return Value("");
        return Value((fs::path(args[0].toStr()) / args[1].toStr()).string());
    }, "joinpath");

    builtins_["type"] = Value::makeNativeFunction([](Interpreter&, vector<Value>& args) {
        if (args.empty()) return Value("null");
        switch (args[0].type) {
            case VT::Null: return Value("null");
            case VT::Bool: return Value("bool");
            case VT::Int: return Value("int");
            case VT::Float: return Value("float");
            case VT::String: return Value("str");
            case VT::List: return Value("list");
            case VT::Dict: return Value("dict");
            case VT::Function: return Value("function");
            case VT::Class: return Value("class");
            case VT::Object: return Value("object");
            case VT::NativeObject: return Value("native");
            case VT::Handle: return Value("handle");
        }
        return Value("unknown");
    }, "type");

    builtins_["sound"] = Value::makeNativeFunction([](Interpreter&, vector<Value>& args) {
        if (args.empty() || args[0].isNull()) {
            cout << '\a';
            cout.flush();
        } else {
            openFileWithDefaultApp(args[0].toStr());
        }
        return Value();
    }, "sound");

    builtins_["zip_folder"] = Value::makeNativeFunction([](Interpreter&, vector<Value>& args) {
        if (args.empty()) return Value(false);
        string path = args[0].toStr();
        int level = args.size() > 1 ? (int)args[1].toInt() : 5;
        bool remove = args.size() > 2 ? args[2].toBool() : false;
        return Value(zipFolder(path, level, remove));
    }, "zip_folder");

    builtins_["list"] = Value::makeNativeFunction([](Interpreter&, vector<Value>& args) {
        if (args.empty()) return makeListFromVector({});
        if (args[0].isList()) return args[0];
        if (args[0].isString()) {
            vector<Value> out;
            for (char c : args[0].s) out.push_back(Value(string(1, c)));
            return makeListFromVector(out);
        }
        if (args[0].isDict()) {
            vector<Value> out;
            for (auto& kv : args[0].dict->items) out.push_back(kv.first);
            return makeListFromVector(out);
        }
        return makeListFromVector({args[0]});
    }, "list");

    builtins_["dict"] = Value::makeNativeFunction([](Interpreter&, vector<Value>& args) {
        if (!args.empty() && args[0].isDict()) return args[0];
        return Value::makeDict(make_shared<Dict>());
    }, "dict");

    builtins_["tuple"] = builtins_["list"];
    builtins_["set"] = builtins_["list"];

    builtins_["range"] = Value::makeNativeFunction([](Interpreter&, vector<Value>& args) {
        long long start = 0, end = 0, step = 1;
        if (args.size() == 1) {
            end = args[0].toInt();
        } else if (args.size() == 2) {
            start = args[0].toInt();
            end = args[1].toInt();
        } else if (args.size() >= 3) {
            start = args[0].toInt();
            end = args[1].toInt();
            step = args[2].toInt();
        }
        vector<Value> out;
        if (step == 0) throw ProstoError{"ValueError", "range step cannot be zero"};
        if (step > 0) for (long long i = start; i < end; i += step) out.push_back(Value(i));
        else for (long long i = start; i > end; i += step) out.push_back(Value(i));
        return makeListFromVector(out);
    }, "range");

    builtins_["sorted"] = Value::makeNativeFunction([](Interpreter&, vector<Value>& args) {
        if (args.empty() || !args[0].isList()) return makeListFromVector({});
        auto out = make_shared<ValueList>(*args[0].list);
        bool rev = args.size() > 1 && args[1].toBool();
        if (rev) sort(out->begin(), out->end(), [](const Value& a, const Value& b) { return lessValue(b, a); });
        else sort(out->begin(), out->end(), lessValue);
        return Value::makeList(out);
    }, "sorted");

    builtins_["enumerate"] = Value::makeNativeFunction([](Interpreter&, vector<Value>& args) {
        vector<Value> out;
        if (!args.empty() && args[0].isList()) {
            long long idx = 0;
            for (auto& v : *args[0].list) {
                out.push_back(makeListFromVector({Value(idx++), v}));
            }
        }
        return makeListFromVector(out);
    }, "enumerate");

    builtins_["zip"] = Value::makeNativeFunction([](Interpreter&, vector<Value>& args) {
        vector<Value> out;
        if (args.empty()) return makeListFromVector(out);
        size_t minLen = SIZE_MAX;
        for (auto& a : args) {
            if (!a.isList()) throw ProstoError{"TypeError", "zip() arguments must be lists"};
            minLen = min(minLen, a.list->size());
        }
        for (size_t i = 0; i < minLen; i++) {
            vector<Value> row;
            for (auto& a : args) row.push_back((*a.list)[i]);
            out.push_back(makeListFromVector(row));
        }
        return makeListFromVector(out);
    }, "zip");

    builtins_["map"] = Value::makeNativeFunction([](Interpreter& in, vector<Value>& args) {
        if (args.size() < 2 || !args[0].isFunction() || !args[1].isList()) return makeListFromVector({});
        vector<Value> out;
        for (auto& x : *args[1].list) {
            vector<Value> callArgs{x};
            out.push_back(in.callValue(args[0], callArgs, nullptr));
        }
        return makeListFromVector(out);
    }, "map");

    builtins_["filter"] = Value::makeNativeFunction([](Interpreter& in, vector<Value>& args) {
        if (args.size() < 2 || !args[0].isFunction() || !args[1].isList()) return makeListFromVector({});
        vector<Value> out;
        for (auto& x : *args[1].list) {
            vector<Value> callArgs{x};
            if (in.callValue(args[0], callArgs, nullptr).toBool()) out.push_back(x);
        }
        return makeListFromVector(out);
    }, "filter");

    builtins_["format"] = Value::makeNativeFunction([](Interpreter&, vector<Value>& args) {
        if (args.empty()) return Value("");
        string fmt = args[0].toStr();
        vector<Value> rest(args.begin() + 1, args.end());
        return Value(formatString(fmt, rest));
    }, "format");

    builtins_["template"] = Value::makeNativeFunction([](Interpreter&, vector<Value>& args) {
        if (args.empty()) return Value("");
        string tmpl = args[0].toStr();
        Value data = args.size() > 1 ? args[1] : Value();
        return Value(templateString(tmpl, data));
    }, "template");

    builtins_["pi"] = Value(std::acos(-1.0));
    builtins_["e"] = Value(std::exp(1.0));

    builtins_["sin"] = Value::makeNativeFunction([](Interpreter&, vector<Value>& args) {
        return Value(sin(args.empty() ? 0.0 : args[0].toFloat()));
    }, "sin");

    builtins_["cos"] = Value::makeNativeFunction([](Interpreter&, vector<Value>& args) {
        return Value(cos(args.empty() ? 0.0 : args[0].toFloat()));
    }, "cos");

    builtins_["tan"] = Value::makeNativeFunction([](Interpreter&, vector<Value>& args) {
        return Value(tan(args.empty() ? 0.0 : args[0].toFloat()));
    }, "tan");

    builtins_["sqrt"] = Value::makeNativeFunction([](Interpreter&, vector<Value>& args) {
        return Value(sqrt(args.empty() ? 0.0 : args[0].toFloat()));
    }, "sqrt");

    builtins_["log"] = Value::makeNativeFunction([](Interpreter&, vector<Value>& args) {
        if (args.empty()) return Value(0.0);
        double x = args[0].toFloat();
        if (args.size() > 1 && !args[1].isNull()) return Value(log(x) / log(args[1].toFloat()));
        return Value(log(x));
    }, "log");

    builtins_["floor"] = Value::makeNativeFunction([](Interpreter&, vector<Value>& args) {
        return Value((long long)floor(args.empty() ? 0.0 : args[0].toFloat()));
    }, "floor");

    builtins_["ceil"] = Value::makeNativeFunction([](Interpreter&, vector<Value>& args) {
        return Value((long long)ceil(args.empty() ? 0.0 : args[0].toFloat()));
    }, "ceil");

    builtins_["pow"] = Value::makeNativeFunction([](Interpreter&, vector<Value>& args) {
        if (args.size() < 2) return Value(0LL);
        return prosto::applyBinary("**", args[0], args[1]);
    }, "pow");

    builtins_["json_parse"] = Value::makeNativeFunction([](Interpreter&, vector<Value>& args) {
        if (args.empty()) return Value();
        try {
            return jsonToValue(json::parse(args[0].toStr()));
        } catch (...) {
            throw ProstoError{"ValueError", "invalid JSON"};
        }
    }, "json_parse");

    builtins_["json_stringify"] = Value::makeNativeFunction([](Interpreter&, vector<Value>& args) {
        if (args.empty()) return Value("null");
        json j = valueToJson(args[0]);
        if (args.size() > 1 && !args[1].isNull()) {
            int indent = (int)args[1].toInt();
            return Value(j.dump(indent));
        }
        return Value(j.dump());
    }, "json_stringify");

    builtins_["http_get"] = Value::makeNativeFunction([](Interpreter&, vector<Value>& args) {
        if (args.empty()) throw ProstoError{"TypeError", "http_get(url) requires url"};
        Value headers = args.size() > 1 ? args[1] : Value();
        int timeout = args.size() > 2 ? (int)args[2].toInt() : 10;
        return doHttpRequest("GET", args[0].toStr(), Value(), headers, timeout);
    }, "http_get");

    builtins_["http_post"] = Value::makeNativeFunction([](Interpreter&, vector<Value>& args) {
        if (args.empty()) throw ProstoError{"TypeError", "http_post(url) requires url"};
        Value data = args.size() > 1 ? args[1] : Value();
        Value headers = args.size() > 2 ? args[2] : Value();
        int timeout = args.size() > 3 ? (int)args[3].toInt() : 10;
        return doHttpRequest("POST", args[0].toStr(), data, headers, timeout);
    }, "http_post");

    builtins_["http_put"] = Value::makeNativeFunction([](Interpreter&, vector<Value>& args) {
        if (args.empty()) throw ProstoError{"TypeError", "http_put(url) requires url"};
        Value data = args.size() > 1 ? args[1] : Value();
        Value headers = args.size() > 2 ? args[2] : Value();
        int timeout = args.size() > 3 ? (int)args[3].toInt() : 10;
        return doHttpRequest("PUT", args[0].toStr(), data, headers, timeout);
    }, "http_put");

    builtins_["http_delete"] = Value::makeNativeFunction([](Interpreter&, vector<Value>& args) {
        if (args.empty()) throw ProstoError{"TypeError", "http_delete(url) requires url"};
        Value headers = args.size() > 1 ? args[1] : Value();
        int timeout = args.size() > 2 ? (int)args[2].toInt() : 10;
        return doHttpRequest("DELETE", args[0].toStr(), Value(), headers, timeout);
    }, "http_delete");

    builtins_["http_download"] = Value::makeNativeFunction([](Interpreter&, vector<Value>& args) {
        if (args.size() < 2) return Value(false);
        int timeout = args.size() > 2 ? (int)args[2].toInt() : 30;
        return Value(httpDownload(args[0].toStr(), args[1].toStr(), timeout));
    }, "http_download");

    builtins_["HttpResponse"] = Value::makeNativeFunction([](Interpreter&, vector<Value>&) {
        return makeHttpResponse(0, {}, "");
    }, "HttpResponse");

    builtins_["RE_IGNORECASE"] = Value(2LL);
    builtins_["RE_MULTILINE"] = Value(8LL);
    builtins_["RE_DOTALL"] = Value(16LL);

    auto makeRegex = [](const string& pattern, int flags) -> regex {
        auto opts = regex::ECMAScript;
        if (flags & 2) opts |= regex::icase;
        return regex(pattern, opts);
    };

    builtins_["regex_match"] = Value::makeNativeFunction([makeRegex](Interpreter&, vector<Value>& args) {
        if (args.size() < 2) return Value(false);
        int flags = args.size() > 2 ? (int)args[2].toInt() : 0;
        return Value(regex_match(args[1].toStr(), makeRegex(args[0].toStr(), flags)));
    }, "regex_match");

    builtins_["regex_fullmatch"] = builtins_["regex_match"];

    builtins_["regex_search"] = Value::makeNativeFunction([makeRegex](Interpreter&, vector<Value>& args) {
        if (args.size() < 2) return Value();
        int flags = args.size() > 2 ? (int)args[2].toInt() : 0;
        smatch m;
        string s = args[1].toStr();
        if (!regex_search(s, m, makeRegex(args[0].toStr(), flags))) return Value();

        vector<Value> groups;
        for (size_t i = 1; i < m.size(); i++) groups.push_back(Value(m[i].str()));

        auto d = make_shared<Dict>();
        d->set(Value("match"), Value(m[0].str()));
        d->set(Value("groups"), makeListFromVector(groups));
        d->set(Value("start"), Value((long long)m.position(0)));
        d->set(Value("end"), Value((long long)(m.position(0) + m.length(0))));
        return Value::makeDict(d);
    }, "regex_search");

    builtins_["regex_findall"] = Value::makeNativeFunction([makeRegex](Interpreter&, vector<Value>& args) {
        if (args.size() < 2) return makeListFromVector({});
        int flags = args.size() > 2 ? (int)args[2].toInt() : 0;
        string s = args[1].toStr();
        auto re = makeRegex(args[0].toStr(), flags);
        auto begin = sregex_iterator(s.begin(), s.end(), re);
        auto end = sregex_iterator();
        vector<Value> out;
        for (auto it = begin; it != end; ++it) {
            if (it->size() > 1) out.push_back(Value((*it)[1].str()));
            else out.push_back(Value(it->str()));
        }
        return makeListFromVector(out);
    }, "regex_findall");

    builtins_["regex_replace"] = Value::makeNativeFunction([makeRegex](Interpreter&, vector<Value>& args) {
        if (args.size() < 3) throw ProstoError{"TypeError", "regex_replace(pattern,replacement,s)"};
        int flags = args.size() > 4 ? (int)args[4].toInt() : 0;
        auto re = makeRegex(args[0].toStr(), flags);
        return Value(regex_replace(args[2].toStr(), re, args[1].toStr()));
    }, "regex_replace");

    builtins_["regex_split"] = Value::makeNativeFunction([makeRegex](Interpreter&, vector<Value>& args) {
        if (args.size() < 2) return makeListFromVector({});
        int flags = args.size() > 2 ? (int)args[2].toInt() : 0;
        string s = args[1].toStr();
        auto re = makeRegex(args[0].toStr(), flags);
        sregex_token_iterator it(s.begin(), s.end(), re, -1), end;
        vector<Value> out;
        for (; it != end; ++it) out.push_back(Value(it->str()));
        return makeListFromVector(out);
    }, "regex_split");

    builtins_["regex_escape"] = Value::makeNativeFunction([](Interpreter&, vector<Value>& args) {
        if (args.empty()) return Value("");
        string s = args[0].toStr();
        string out;
        for (char c : s) {
            if (strchr(".^$+()[]{}|\\?*", c)) out += "\\";
            out += c;
        }
        return Value(out);
    }, "regex_escape");

    builtins_["base64_encode"] = Value::makeNativeFunction([](Interpreter&, vector<Value>& args) {
        if (args.empty()) return Value("");
        string s = args[0].toStr();
        return Value(base64EncodeImpl((const unsigned char*)s.data(), s.size(), false));
    }, "base64_encode");

    builtins_["base64_decode"] = Value::makeNativeFunction([](Interpreter&, vector<Value>& args) {
        if (args.empty()) return Value("");
        return Value(base64DecodeImpl(args[0].toStr(), false));
    }, "base64_decode");

    builtins_["base64url_encode"] = Value::makeNativeFunction([](Interpreter&, vector<Value>& args) {
        if (args.empty()) return Value("");
        string s = args[0].toStr();
        return Value(base64EncodeImpl((const unsigned char*)s.data(), s.size(), true));
    }, "base64url_encode");

    builtins_["base64url_decode"] = Value::makeNativeFunction([](Interpreter&, vector<Value>& args) {
        if (args.empty()) return Value("");
        return Value(base64DecodeImpl(args[0].toStr(), true));
    }, "base64url_decode");

    builtins_["hex_encode"] = Value::makeNativeFunction([](Interpreter&, vector<Value>& args) {
        if (args.empty()) return Value("");
        return Value(hexEncodeString(args[0].toStr()));
    }, "hex_encode");

    builtins_["hex_decode"] = Value::makeNativeFunction([](Interpreter&, vector<Value>& args) {
        if (args.empty()) return Value("");
        return Value(hexDecodeString(args[0].toStr()));
    }, "hex_decode");

    builtins_["url_encode"] = Value::makeNativeFunction([](Interpreter&, vector<Value>& args) {
        if (args.empty()) return Value("");
        return Value(urlEncodeString(args[0].toStr()));
    }, "url_encode");

    builtins_["url_decode"] = Value::makeNativeFunction([](Interpreter&, vector<Value>& args) {
        if (args.empty()) return Value("");
        return Value(urlDecodeString(args[0].toStr()));
    }, "url_decode");

    builtins_["html_escape"] = Value::makeNativeFunction([](Interpreter&, vector<Value>& args) {
        if (args.empty()) return Value("");
        return Value(htmlEscapeString(args[0].toStr()));
    }, "html_escape");

    builtins_["html_unescape"] = Value::makeNativeFunction([](Interpreter&, vector<Value>& args) {
        if (args.empty()) return Value("");
        return Value(htmlUnescapeString(args[0].toStr()));
    }, "html_unescape");

    builtins_["utf8_bytes"] = Value::makeNativeFunction([](Interpreter&, vector<Value>& args) {
        vector<Value> out;
        if (!args.empty()) {
            string s = args[0].toStr();
            for (unsigned char c : s) out.push_back(Value((long long)c));
        }
        return makeListFromVector(out);
    }, "utf8_bytes");

    builtins_["utf8_str"] = Value::makeNativeFunction([](Interpreter&, vector<Value>& args) {
        string out;
        if (!args.empty() && args[0].isList()) {
            for (auto& v : *args[0].list) out += (char)(v.toInt() & 0xFF);
        }
        return Value(out);
    }, "utf8_str");

    builtins_["md5"] = Value::makeNativeFunction([](Interpreter&, vector<Value>& args) {
        if (args.empty()) return Value("");
        return Value(evpDigestHex("md5", (const unsigned char*)args[0].toStr().data(), args[0].toStr().size()));
    }, "md5");

    builtins_["sha1"] = Value::makeNativeFunction([](Interpreter&, vector<Value>& args) {
        if (args.empty()) return Value("");
        return Value(evpDigestHex("sha1", (const unsigned char*)args[0].toStr().data(), args[0].toStr().size()));
    }, "sha1");

    builtins_["sha256"] = Value::makeNativeFunction([](Interpreter&, vector<Value>& args) {
        if (args.empty()) return Value("");
        return Value(evpDigestHex("sha256", (const unsigned char*)args[0].toStr().data(), args[0].toStr().size()));
    }, "sha256");

    builtins_["sha512"] = Value::makeNativeFunction([](Interpreter&, vector<Value>& args) {
        if (args.empty()) return Value("");
        return Value(evpDigestHex("sha512", (const unsigned char*)args[0].toStr().data(), args[0].toStr().size()));
    }, "sha512");

    builtins_["crc32"] = Value::makeNativeFunction([](Interpreter&, vector<Value>& args) {
        if (args.empty()) return Value(0LL);
        return Value((long long)crc32String(args[0].toStr()));
    }, "crc32");

    builtins_["hmac_sha256"] = Value::makeNativeFunction([](Interpreter&, vector<Value>& args) {
        if (args.size() < 2) return Value("");
        return Value(hmacSha256Hex(args[0].toStr(), args[1].toStr()));
    }, "hmac_sha256");

    builtins_["hash_file"] = Value::makeNativeFunction([](Interpreter&, vector<Value>& args) {
        if (args.empty()) return Value("");
        string algo = args.size() > 1 ? args[1].toStr() : "sha256";
        return Value(evpFileDigestHex(args[0].toStr(), algo));
    }, "hash_file");

    builtins_["random"] = Value::makeNativeFunction([](Interpreter&, vector<Value>&) {
        uniform_real_distribution<double> dist(0.0, 1.0);
        return Value(dist(globalRng()));
    }, "random");

    builtins_["randint"] = Value::makeNativeFunction([](Interpreter&, vector<Value>& args) {
        if (args.size() < 2) return Value(0LL);
        uniform_int_distribution<long long> dist(args[0].toInt(), args[1].toInt());
        return Value(dist(globalRng()));
    }, "randint");

    builtins_["randfloat"] = Value::makeNativeFunction([](Interpreter&, vector<Value>& args) {
        if (args.size() < 2) return Value(0.0);
        uniform_real_distribution<double> dist(args[0].toFloat(), args[1].toFloat());
        return Value(dist(globalRng()));
    }, "randfloat");

    builtins_["choice"] = Value::makeNativeFunction([](Interpreter&, vector<Value>& args) {
        if (args.empty() || !args[0].isList() || args[0].list->empty()) return Value();
        uniform_int_distribution<size_t> dist(0, args[0].list->size() - 1);
        return (*args[0].list)[dist(globalRng())];
    }, "choice");

    builtins_["sample"] = Value::makeNativeFunction([](Interpreter&, vector<Value>& args) {
        if (args.empty() || !args[0].isList()) return makeListFromVector({});
        long long k = args.size() > 1 ? args[1].toInt() : 0;
        auto src = *args[0].list;
        shuffle(src.begin(), src.end(), globalRng());
        if (k > (long long)src.size()) k = (long long)src.size();
        if (k < 0) k = 0;
        src.resize(k);
        return makeListFromVector(src);
    }, "sample");

    builtins_["shuffle"] = Value::makeNativeFunction([](Interpreter&, vector<Value>& args) {
        if (!args.empty() && args[0].isList()) {
            shuffle(args[0].list->begin(), args[0].list->end(), globalRng());
            return args[0];
        }
        return makeListFromVector({});
    }, "shuffle");

    builtins_["seed"] = Value::makeNativeFunction([](Interpreter&, vector<Value>& args) {
        if (!args.empty()) globalRng().seed((uint64_t)args[0].toInt());
        return Value();
    }, "seed");

    builtins_["randbytes"] = Value::makeNativeFunction([](Interpreter&, vector<Value>& args) {
        long long n = args.empty() ? 0 : args[0].toInt();
        uniform_int_distribution<int> dist(0, 255);
        string out;
        for (long long i = 0; i < n; i++) out += (char)dist(globalRng());
        return Value(hexEncodeString(out));
    }, "randbytes");

    builtins_["uuid"] = Value::makeNativeFunction([](Interpreter&, vector<Value>&) {
        return Value(generateUUID());
    }, "uuid");

    builtins_["db_open"] = Value::makeNativeFunction([](Interpreter&, vector<Value>& args) {
        string path = args.empty() ? ":memory:" : args[0].toStr();
        sqlite3* db = nullptr;
        if (sqlite3_open(path.c_str(), &db) != SQLITE_OK) {
            string msg = sqlite3_errmsg(db);
            sqlite3_close(db);
            throw ProstoError{"RuntimeError", "sqlite open failed: " + msg};
        }
        shared_ptr<sqlite3> sp(db, [](sqlite3* p) { if (p) sqlite3_close(p); });
        return Value::makeHandle("sqlite3", sp);
    }, "db_open");

    auto getDb = [](Value& v) -> shared_ptr<sqlite3> {
        if (!v.isHandle() || v.handleType != "sqlite3") {
            throw ProstoError{"TypeError", "not a sqlite connection"};
        }
        return any_cast<shared_ptr<sqlite3>>(v.handle);
    };

    auto bindParams = [](sqlite3_stmt* st, vector<Value>& params) {
        for (size_t i = 0; i < params.size(); i++) {
            int idx = (int)i + 1;
            Value& v = params[i];
            if (v.isNull()) sqlite3_bind_null(st, idx);
            else if (v.type == VT::Int || v.type == VT::Bool) sqlite3_bind_int64(st, idx, v.toInt());
            else if (v.type == VT::Float) sqlite3_bind_double(st, idx, v.toFloat());
            else {
                string s = v.toStr();
                sqlite3_bind_text(st, idx, s.c_str(), -1, SQLITE_TRANSIENT);
            }
        }
    };

    builtins_["db_exec"] = Value::makeNativeFunction([getDb, bindParams](Interpreter&, vector<Value>& args) {
        if (args.size() < 2) throw ProstoError{"TypeError", "db_exec(conn, sql[, params])"};
        auto db = getDb(args[0]);
        string sql = args[1].toStr();
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db.get(), sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
            throw ProstoError{"RuntimeError", sqlite3_errmsg(db.get())};
        }

        vector<Value> params;
        if (args.size() > 2) {
            if (args[2].isList()) params = *args[2].list;
            else params.push_back(args[2]);
        }
        bindParams(st, params);

        int rc = sqlite3_step(st);
        sqlite3_finalize(st);
        if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
            throw ProstoError{"RuntimeError", sqlite3_errmsg(db.get())};
        }
        return Value((long long)sqlite3_changes(db.get()));
    }, "db_exec");

    builtins_["db_query"] = Value::makeNativeFunction([getDb, bindParams](Interpreter&, vector<Value>& args) {
        if (args.size() < 2) throw ProstoError{"TypeError", "db_query(conn, sql[, params])"};
        auto db = getDb(args[0]);
        string sql = args[1].toStr();
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db.get(), sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
            throw ProstoError{"RuntimeError", sqlite3_errmsg(db.get())};
        }

        vector<Value> params;
        if (args.size() > 2) {
            if (args[2].isList()) params = *args[2].list;
            else params.push_back(args[2]);
        }
        bindParams(st, params);

        vector<Value> rows;
        int cols = sqlite3_column_count(st);
        while (sqlite3_step(st) == SQLITE_ROW) {
            auto d = make_shared<Dict>();
            for (int c = 0; c < cols; c++) {
                string name = sqlite3_column_name(st, c);
                int type = sqlite3_column_type(st, c);
                Value val;
                if (type == SQLITE_INTEGER) val = Value((long long)sqlite3_column_int64(st, c));
                else if (type == SQLITE_FLOAT) val = Value(sqlite3_column_double(st, c));
                else if (type == SQLITE_TEXT) val = Value((const char*)sqlite3_column_text(st, c));
                else if (type == SQLITE_NULL) val = Value();
                else val = Value((const char*)sqlite3_column_text(st, c));
                d->set(Value(name), val);
            }
            rows.push_back(Value::makeDict(d));
        }

        sqlite3_finalize(st);
        return makeListFromVector(rows);
    }, "db_query");

    builtins_["db_close"] = Value::makeNativeFunction([getDb](Interpreter&, vector<Value>& args) {
        if (!args.empty()) {
            auto db = getDb(args[0]);
            sqlite3_close(db.get());
        }
        return Value();
    }, "db_close");

    builtins_["mkdir"] = Value::makeNativeFunction([](Interpreter&, vector<Value>& args) {
        if (!args.empty()) fs::create_directories(args[0].toStr());
        return Value();
    }, "mkdir");

    builtins_["rmdir"] = Value::makeNativeFunction([](Interpreter&, vector<Value>& args) {
        if (!args.empty()) {
            error_code ec;
            fs::remove_all(args[0].toStr(), ec);
        }
        return Value();
    }, "rmdir");

    builtins_["isfile"] = Value::makeNativeFunction([](Interpreter&, vector<Value>& args) {
        if (args.empty()) return Value(false);
        return Value(fs::is_regular_file(args[0].toStr()));
    }, "isfile");

    builtins_["isdir"] = Value::makeNativeFunction([](Interpreter&, vector<Value>& args) {
        if (args.empty()) return Value(false);
        return Value(fs::is_directory(args[0].toStr()));
    }, "isdir");

    builtins_["exists"] = Value::makeNativeFunction([](Interpreter&, vector<Value>& args) {
        if (args.empty()) return Value(false);
        return Value(fs::exists(args[0].toStr()));
    }, "exists");

    builtins_["abspath"] = Value::makeNativeFunction([](Interpreter&, vector<Value>& args) {
        if (args.empty()) return Value("");
        return Value(fs::absolute(args[0].toStr()).string());
    }, "abspath");

    builtins_["listdir"] = Value::makeNativeFunction([](Interpreter&, vector<Value>& args) {
        string path = args.empty() ? "." : args[0].toStr();
        vector<Value> out;
        if (fs::is_directory(path)) {
            vector<string> names;
            for (auto& e : fs::directory_iterator(path)) names.push_back(e.path().filename().string());
            sort(names.begin(), names.end());
            for (auto& n : names) out.push_back(Value(n));
        }
        return makeListFromVector(out);
    }, "listdir");

    builtins_["glob"] = Value::makeNativeFunction([](Interpreter&, vector<Value>& args) {
        vector<Value> out;
        if (!args.empty()) {
            for (auto& p : globFiles(args[0].toStr())) out.push_back(Value(p));
        }
        return makeListFromVector(out);
    }, "glob");

    builtins_["filesize"] = Value::makeNativeFunction([](Interpreter&, vector<Value>& args) {
        if (args.empty()) return Value(0LL);
        error_code ec;
        auto sz = fs::file_size(args[0].toStr(), ec);
        return Value(ec ? 0LL : (long long)sz);
    }, "filesize");

    builtins_["copyfile"] = Value::makeNativeFunction([](Interpreter&, vector<Value>& args) {
        if (args.size() < 2) throw ProstoError{"TypeError", "copyfile(src,dst)"};
        fs::copy_file(args[0].toStr(), args[1].toStr(), fs::copy_options::overwrite_existing);
        return Value();
    }, "copyfile");

    builtins_["movefile"] = Value::makeNativeFunction([](Interpreter&, vector<Value>& args) {
        if (args.size() < 2) throw ProstoError{"TypeError", "movefile(src,dst)"};
        fs::rename(args[0].toStr(), args[1].toStr());
        return Value();
    }, "movefile");

    builtins_["search"] = Value::makeNativeFunction([](Interpreter&, vector<Value>& args) {
        if (args.empty()) return makeListFromVector({});
        Value recursive = args.size() > 1 ? args[1] : Value(false);
        return searchFiles(args[0].toStr(), recursive);
    }, "search");

    builtins_["EFCObject"] = Value::makeNativeFunction([](Interpreter&, vector<Value>& args) {
        if (args.empty()) throw ProstoError{"TypeError", "EFCObject(path) requires path"};
        return makeEFCObject(args[0].toStr());
    }, "EFCObject");

    builtins_["exec_cmd"] = Value::makeNativeFunction([](Interpreter&, vector<Value>& args) {
        if (args.empty()) throw ProstoError{"TypeError", "exec_cmd(cmd[, cwd])"};
        string cmd = args[0].toStr();
        string cwd = args.size() > 1 && !args[1].isNull() ? args[1].toStr() : "";

        string out = (fs::temp_directory_path() / ("prostop_out_" + generateUUID())).string();
        string err = (fs::temp_directory_path() / ("prostop_err_" + generateUUID())).string();

        auto argv = splitCommandLine(cmd);
        int code = spawnProcess(argv, cwd, out, err);

        string stdoutStr, stderrStr;
        if (fs::exists(out)) stdoutStr = readFileAll(out);
        if (fs::exists(err)) stderrStr = readFileAll(err);

        error_code ec;
        fs::remove(out, ec);
        fs::remove(err, ec);

        auto d = make_shared<Dict>();
        d->set(Value("stdout"), Value(stdoutStr));
        d->set(Value("stderr"), Value(stderrStr));
        d->set(Value("code"), Value((long long)code));
        return Value::makeDict(d);
    }, "exec_cmd");

    builtins_["env_get"] = Value::makeNativeFunction([](Interpreter&, vector<Value>& args) {
        if (args.empty()) return Value("");
        const char* v = getenv(args[0].toStr().c_str());
        if (v) return Value(string(v));
        return args.size() > 1 ? args[1] : Value("");
    }, "env_get");

    builtins_["env_set"] = Value::makeNativeFunction([](Interpreter&, vector<Value>& args) {
        if (args.size() < 2) return Value();
#ifdef _WIN32
        _putenv_s(args[0].toStr().c_str(), args[1].toStr().c_str());
#else
        setenv(args[0].toStr().c_str(), args[1].toStr().c_str(), 1);
#endif
        return Value();
    }, "env_set");

    builtins_["env_all"] = Value::makeNativeFunction([](Interpreter&, vector<Value>&) {
        auto d = make_shared<Dict>();
#ifdef _WIN32
        char** env = _environ;
#else
        extern char** environ;
        char** env = environ;
#endif
        for (char** p = env; p && *p; p++) {
            string s(*p);
            size_t pos = s.find('=');
            if (pos != string::npos) {
                d->set(Value(s.substr(0, pos)), Value(s.substr(pos + 1)));
            }
        }
        return Value::makeDict(d);
    }, "env_all");

    builtins_["sleep"] = Value::makeNativeFunction([](Interpreter&, vector<Value>& args) {
        double sec = args.empty() ? 0.0 : args[0].toFloat();
        this_thread::sleep_for(chrono::duration<double>(sec));
        return Value();
    }, "sleep");

    builtins_["cwd"] = Value::makeNativeFunction([](Interpreter&, vector<Value>&) {
        return Value(fs::current_path().string());
    }, "cwd");

    builtins_["chdir"] = Value::makeNativeFunction([](Interpreter&, vector<Value>& args) {
        if (!args.empty()) fs::current_path(args[0].toStr());
        return Value();
    }, "chdir");

    builtins_["tui_clear"] = Value::makeNativeFunction([](Interpreter&, vector<Value>&) {
        cout << "\033[2J\033[H";
        cout.flush();
        return Value();
    }, "tui_clear");

    auto ansiColor = [](const string& color) -> string {
        if (color == "red") return "\033[31m";
        if (color == "green") return "\033[32m";
        if (color == "yellow") return "\033[33m";
        if (color == "blue") return "\033[34m";
        if (color == "magenta") return "\033[35m";
        if (color == "cyan") return "\033[36m";
        return "\033[37m";
    };

    auto ansiBg = [](const string& color) -> string {
        if (color == "red") return "\033[41m";
        if (color == "green") return "\033[42m";
        if (color == "yellow") return "\033[43m";
        if (color == "blue") return "\033[44m";
        if (color == "magenta") return "\033[45m";
        if (color == "cyan") return "\033[46m";
        return "";
    };

    builtins_["tui_color"] = Value::makeNativeFunction([ansiColor, ansiBg](Interpreter&, vector<Value>& args) {
        if (args.empty()) return Value("");
        string text = args[0].toStr();
        string color = args.size() > 1 ? args[1].toStr() : "white";
        bool bold = args.size() > 2 ? args[2].toBool() : false;
        string bg = args.size() > 3 && !args[3].isNull() ? args[3].toStr() : "";

        string out;
        if (!bg.empty()) out += ansiBg(bg);
        if (bold) out += "\033[1m";
        out += ansiColor(color);
        out += text;
        out += "\033[0m";
        return Value(out);
    }, "tui_color");

    builtins_["tui_box"] = Value::makeNativeFunction([](Interpreter&, vector<Value>& args) {
        string title = args.size() > 0 ? args[0].toStr() : "";
        vector<string> lines;
        if (args.size() > 1 && args[1].isList()) {
            for (auto& x : *args[1].list) lines.push_back(x.toStr());
        } else if (args.size() > 1) {
            lines.push_back(args[1].toStr());
        }
        long long width = args.size() > 2 ? args[2].toInt() : 40;
        width = max<long long>(width, (long long)title.size() + 6);

        auto repeat = [&](const string& s, size_t count) {
            string out;
            out.reserve(s.size() * count);
            while (count--) out += s;
            return out;
        };

        string top;
        if (!title.empty()) {
            top = "+- " + title + " " + repeat("-", (size_t)max<long long>(0, width - (long long)title.size() - 4)) + " -+";
        } else {
            top = "+" + repeat("-", (size_t)width) + "+";
        }
        string bottom = "+" + repeat("-", (size_t)width) + "+";

        string out = top + "\n";
        for (auto& line : lines) {
            long long pad = max<long long>(0, width - (long long)line.size() - 2);
            out += "| " + line + string((size_t)pad, ' ') + " |\n";
        }
        out += bottom;
        return Value(out);
    }, "tui_box");

    builtins_["tui_table"] = Value::makeNativeFunction([](Interpreter&, vector<Value>& args) {
        if (args.size() < 2 || !args[0].isList() || !args[1].isList()) return Value("");
        long long colWidth = args.size() > 2 ? args[2].toInt() : 15;

        auto repeat = [&](const std::string& s, size_t count) {
            std::string out;
            out.reserve(s.size() * count);
            while (count--) out += s;
            return out;
        };

        auto center = [&](const std::string& s) {
            long long pad = max<long long>(0, colWidth - (long long)s.size());
            long long left = pad / 2;
            long long right = pad - left;
            return std::string((size_t)left, ' ') + s + std::string((size_t)right, ' ');
        };

        auto row = [&](const vector<string>& cells) {
            std::string out = "| ";
            for (size_t i = 0; i < cells.size(); i++) {
                out += center(cells[i]);
                out += " |";
            }
            out += "\n";
            return out;
        };

        vector<string> headers;
        for (auto& h : *args[0].list) headers.push_back(h.toStr());

        std::string top = "+";
        std::string sep = "+";
        std::string bot = "+";
        for (size_t i = 0; i < headers.size(); i++) {
            top += repeat("-", (size_t)colWidth) + "+";
            sep += repeat("-", (size_t)colWidth) + "+";
            bot += repeat("-", (size_t)colWidth) + "+";
        }

        std::string out = top + "\n" + row(headers) + "\n" + sep + "\n";
        for (auto& rowValue : *args[1].list) {
            vector<string> cells;
            if (rowValue.isList()) {
                for (auto& rv : *rowValue.list) cells.push_back(rv.toStr());
            } else {
                cells.push_back(rowValue.toStr());
            }
            if (cells.size() < headers.size()) cells.resize(headers.size());
            out += row(cells) + "\n";
        }
        out += bot;
        return Value(out);
    }, "tui_table");

    builtins_["tui_progress"] = Value::makeNativeFunction([](Interpreter&, vector<Value>& args) {
        long long current = args.size() > 0 ? args[0].toInt() : 0;
        long long total = args.size() > 1 ? args[1].toInt() : 0;
        long long width = args.size() > 2 ? args[2].toInt() : 30;
        string label = args.size() > 3 ? args[3].toStr() : "";

        double pct = total > 0 ? (double)current / (double)total : 0.0;
        long long filled = (long long)(width * pct);
        auto repeat = [&](const string& s, size_t count) {
            string out;
            out.reserve(s.size() * count);
            while (count--) out += s;
            return out;
        };
        string bar = repeat("#", (size_t)filled) + repeat(".", (size_t)max<long long>(0, width - filled));
        ostringstream oss;
        oss << label << " [" << bar << "] " << (int)(pct * 100) << "%";
        return Value(oss.str());
    }, "tui_progress");

    builtins_["tui_bar"] = Value::makeNativeFunction([](Interpreter&, vector<Value>& args) {
        string label = args.size() > 0 ? args[0].toStr() : "";
        long long value = args.size() > 1 ? args[1].toInt() : 0;
        long long maxVal = args.size() > 2 ? args[2].toInt() : 0;
        long long width = args.size() > 3 ? args[3].toInt() : 20;

        auto repeat = [&](const std::string& s, size_t count) {
            std::string out;
            out.reserve(s.size() * count);
            while (count--) out += s;
            return out;
        };

        double pct = maxVal > 0 ? (double)value / (double)maxVal : 0.0;
        long long filled = max<long long>(0, min<long long>(width, (long long)(pct * width)));
        string bar = repeat("#", (size_t)filled) + repeat(".", (size_t)max<long long>(0, width - filled));
        ostringstream oss;
        if (!label.empty()) oss << label << " ";
        oss << "[" << bar << "] ";
        oss << (int)(pct * 100) << "%";
        return Value(oss.str());
    }, "tui_bar");

    builtins_["tui_menu"] = Value::makeNativeFunction([](Interpreter& in, vector<Value>& args) {
        string title = args.size() > 0 ? args[0].toStr() : "";
        vector<Value> opts;
        if (args.size() > 1 && args[1].isList()) opts = *args[1].list;

        vector<Value> lines;
        for (size_t i = 0; i < opts.size(); i++) {
            lines.push_back(Value(to_string(i + 1) + ". " + opts[i].toStr()));
        }

        vector<Value> boxArgs{Value(title), makeListFromVector(lines)};
        Value box = in.builtins_["tui_box"].func->nativeFn(in, boxArgs);
        cout << box.toStr() << endl;

        while (true) {
            cout << "Select: ";
            cout.flush();
            string s;
            getline(cin, s);
            try {
                long long idx = stoll(trim(s)) - 1;
                if (idx >= 0 && idx < (long long)opts.size()) return Value(idx);
            } catch (...) {}
            cout << "Invalid choice." << endl;
        }
    }, "tui_menu");

    builtins_["tui_confirm"] = Value::makeNativeFunction([](Interpreter&, vector<Value>& args) {
        string prompt = args.empty() ? "Continue?" : args[0].toStr();
        cout << prompt << " [y/N]: ";
        cout.flush();
        string s;
        getline(cin, s);
        s = lowerString(trim(s));
        return Value(s == "y" || s == "yes");
    }, "tui_confirm");

    builtins_["tui_cursor"] = Value::makeNativeFunction([](Interpreter&, vector<Value>& args) {
        long long x = args.size() > 0 ? args[0].toInt() : 1;
        long long y = args.size() > 1 ? args[1].toInt() : 1;
        cout << "\033[" << y << ";" << x << "H";
        cout.flush();
        return Value();
    }, "tui_cursor");

    builtins_["tui_hide_cursor"] = Value::makeNativeFunction([](Interpreter&, vector<Value>&) {
        cout << "\033[?25l";
        cout.flush();
        return Value();
    }, "tui_hide_cursor");

    builtins_["tui_show_cursor"] = Value::makeNativeFunction([](Interpreter&, vector<Value>&) {
        cout << "\033[?25h";
        cout.flush();
        return Value();
    }, "tui_show_cursor");

    builtins_["tui_title"] = Value::makeNativeFunction([](Interpreter&, vector<Value>& args) {
        if (!args.empty()) cout << "\033]0;" << args[0].toStr() << "\007";
        cout.flush();
        return Value();
    }, "tui_title");

    builtins_["tui_spinner"] = Value::makeNativeFunction([](Interpreter&, vector<Value>& args) {
        string label = args.empty() ? "" : args[0].toStr();
        double duration = args.size() > 1 ? args[1].toFloat() : 2.0;
        vector<string> frames = {"|", "/", "-", "\\"};
        auto end = chrono::steady_clock::now() + chrono::milliseconds((long long)(duration * 1000));
        size_t i = 0;
        while (chrono::steady_clock::now() < end) {
            cout << "\r" << frames[i % frames.size()] << " " << label;
            cout.flush();
            this_thread::sleep_for(chrono::milliseconds(80));
            i++;
        }
        cout << "\r " << label << "     " << endl;
        return Value();
    }, "tui_spinner");
}

} // namespace prosto
