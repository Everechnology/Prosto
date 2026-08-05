#include "prosto/interpreter.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <numeric>
#include <random>
#include <regex>
#include <sstream>
#include <sys/stat.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <winhttp.h>
#include <bcrypt.h>
#else
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#include <curl/curl.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#endif

#include <nlohmann/json.hpp>
#include "sqlite3.h"

using json = nlohmann::json;
namespace fs = std::filesystem;

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#ifndef M_E
#define M_E 2.71828182845904523536
#endif

namespace prosto {

// thread_local RNG — safe under concurrent access from multiple threads
thread_local std::mt19937_64 tlsRng(
    static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()));

std::mt19937_64& rng() { return tlsRng; }

// ---------------------------------------------------------------------------
// RAII wrappers for external resources
// ---------------------------------------------------------------------------
#ifndef _WIN32
struct CurlGlobalInit {
    CurlGlobalInit() { curl_global_init(CURL_GLOBAL_DEFAULT); }
    ~CurlGlobalInit() { curl_global_cleanup(); }
};
const CurlGlobalInit g_curlInit;

struct CurlEasy {
    CURL* h = curl_easy_init();
    ~CurlEasy() { if (h) curl_easy_cleanup(h); }
    explicit operator bool() const { return h != nullptr; }
    CURL* get() const { return h; }
};

struct CurlSlist {
    curl_slist* list = nullptr;
    ~CurlSlist() { if (list) curl_slist_free_all(list); }
    void append(const std::string& header) {
        list = curl_slist_append(list, header.c_str());
    }
    curl_slist* get() const { return list; }
};
#endif

struct SqliteDb {
    sqlite3* db = nullptr;
    explicit SqliteDb(const std::string& path) {
        if (sqlite3_open(path.c_str(), &db) != SQLITE_OK) {
            std::string msg = sqlite3_errmsg(db);
            sqlite3_close(db);
            db = nullptr;
            throw ProstoError{"RuntimeError", "sqlite open failed: " + msg};
        }
    }
    ~SqliteDb() { if (db) sqlite3_close(db); }
    sqlite3* get() const { return db; }
};

struct SqliteStmt {
    sqlite3_stmt* st = nullptr;
    SqliteStmt(sqlite3* db, const std::string& sql) {
        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
            throw ProstoError{"RuntimeError", sqlite3_errmsg(db)};
        }
    }
    ~SqliteStmt() { if (st) sqlite3_finalize(st); }
    sqlite3_stmt* get() const { return st; }
};

struct HttpResponseNative : NativeObject {
    long status = 0;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;

    HttpResponseNative(long st, std::vector<std::pair<std::string, std::string>> hd, std::string bd)
        : status(st), headers(std::move(hd)), body(std::move(bd)) {}

    Value getAttr(Interpreter&, const std::string& name) override {
        if (name == "status") return Value(static_cast<long long>(status));
        if (name == "body") return Value(body);
        if (name == "headers") {
            auto d = std::make_shared<Dict>();
            for (auto& h : headers) d->set(Value(h.first), Value(h.second));
            return Value::makeDict(d);
        }
        throw ProstoError{"AttributeError", "HttpResponse has no attribute '" + name + "'"};
    }

    std::string repr() const override { return "<HttpResponse " + std::to_string(status) + ">"; }
};

#ifdef _WIN32
struct WinHttpHandle {
    HINTERNET h = nullptr;
    explicit WinHttpHandle(HINTERNET handle) : h(handle) {}
    ~WinHttpHandle() { if (h) WinHttpCloseHandle(h); }
    explicit operator bool() const { return h != nullptr; }
    HINTERNET get() const { return h; }
};
#else
struct EvpMdCtx {
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    ~EvpMdCtx() { if (ctx) EVP_MD_CTX_free(ctx); }
    EVP_MD_CTX* get() const { return ctx; }
};
#endif

static std::string hexEncode(const unsigned char* data, size_t len) {
    static const char* tbl = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; i++) {
        out += tbl[data[i] >> 4];
        out += tbl[data[i] & 15];
    }
    return out;
}


// ---------------------------------------------------------------------------
// Value factories & methods
// ---------------------------------------------------------------------------
Value Value::makeList(std::shared_ptr<ValueList> v) {
    Value x; x.type = VT::List; x.list = std::move(v); return x;
}
Value Value::makeDict(std::shared_ptr<Dict> v) {
    Value x; x.type = VT::Dict; x.dict = std::move(v); return x;
}
Value Value::makeFunction(std::shared_ptr<Function> v) {
    Value x; x.type = VT::Function; x.func = std::move(v); return x;
}
Value Value::makeClass(std::shared_ptr<Class> v) {
    Value x; x.type = VT::Class; x.cls = std::move(v); return x;
}
Value Value::makeObject(std::shared_ptr<Object> v) {
    Value x; x.type = VT::Object; x.obj = std::move(v); return x;
}
Value Value::makeNativeObject(std::shared_ptr<NativeObject> v) {
    Value x; x.type = VT::NativeObject; x.nat = std::move(v); return x;
}
Value Value::makeHandle(const std::string& ht, std::any h) {
    Value x; x.type = VT::Handle; x.handleType = ht; x.handle = std::move(h); return x;
}
Value Value::makeNativeFunction(NativeFn fn, const std::string& name) {
    auto f = std::make_shared<Function>();
    f->native = true;
    f->name = name;
    f->nativeFn = std::move(fn);
    return makeFunction(f);
}

bool Value::toBool() const {
    switch (type) {
        case VT::Null: return false;
        case VT::Bool: return b;
        case VT::Int: return i != 0;
        case VT::Float: return f != 0.0;
        case VT::String: return !s.empty();
        case VT::List: return list && !list->empty();
        case VT::Dict: return dict && !dict->items.empty();
        case VT::Function: return true;
        case VT::Class: return true;
        case VT::Object: return true;
        case VT::NativeObject: return true;
        case VT::Handle: return true;
    }
    return false;
}


Value Value::getAttr(Interpreter& interp, const std::string& name) const {
    if (type == VT::Dict) return dict ? dict->get(Value(name), Value()) : Value();
    return interp.getAttr(*this, name);
}

void Value::setAttr(Interpreter& interp, const std::string& name, const Value& v) {
    if (type == VT::Dict) { if (dict) dict->set(Value(name), v); return; }
    interp.setAttr(*const_cast<Value*>(this), name, v);
}

Value Value::call(Interpreter& interp, const std::vector<Value>& args) const {
    std::vector<Value> copy(args.begin(), args.end());
    return interp.callValue(*this, copy, nullptr);
}

Value Value::index(Interpreter& interp, const Value& idx) const {
    return interp.indexValue(*this, idx);
}

Value Value::add(const Value& v) const {
    switch (type) {
        case VT::Int: return Value(i + v.toInt());
        case VT::Float: return Value(f + v.toFloat());
        case VT::String: return Value(s + v.toString());
        case VT::List: {
            if (!v.isList() || !v.list) throw ProstoError{"TypeError", "unsupported operand type(s) for +: '" + typeStr() + "' and '" + v.typeStr() + "'"};
            auto l = std::make_shared<ValueList>(*list);
            l->insert(l->end(), v.list->begin(), v.list->end());
            return Value::makeList(l);
        }
        default: throw ProstoError{"TypeError", "unsupported operand type(s) for +: '" + typeStr() + "' and '" + v.typeStr() + "'"};
    }
}

Value Value::sub(const Value& v) const {
    switch (type) {
        case VT::Int: return Value(i - v.toInt());
        case VT::Float: return Value(f - v.toFloat());
        default: throw ProstoError{"TypeError", "unsupported operand type(s) for -: '" + typeStr() + "' and '" + v.typeStr() + "'"};
    }
}

Value Value::mul(const Value& v) const {
    switch (type) {
        case VT::Int: return Value(i * v.toInt());
        case VT::Float: return Value(f * v.toFloat());
        case VT::String: return Value(std::string(i * v.toInt(), s[0]));
        case VT::List: {
            auto l = std::make_shared<ValueList>();
            for (int j = 0; j < v.toInt(); j++) l->insert(l->end(), list->begin(), list->end());
            return Value::makeList(l);
        }
        default: throw ProstoError{"TypeError", "unsupported operand type(s) for *: '" + typeStr() + "' and '" + v.typeStr() + "'"};
    }
}

Value Value::div(const Value& v) const {
    switch (type) {
        case VT::Int: return Value(i / v.toInt());
        case VT::Float: return Value(f / v.toFloat());
        default: throw ProstoError{"TypeError", "unsupported operand type(s) for /: '" + typeStr() + "' and '" + v.typeStr() + "'"};
    }
}

Value Value::mod(const Value& v) const {
    switch (type) {
        case VT::Int: return Value(i % v.toInt());
        default: throw ProstoError{"TypeError", "unsupported operand type(s) for %: '" + typeStr() + "' and '" + v.typeStr() + "'"};
    }
}

Value Value::pow(const Value& v) const {
    switch (type) {
        case VT::Int: return Value(static_cast<int>(std::pow(i, v.toInt())));
        case VT::Float: return Value(std::pow(f, v.toFloat()));
        default: throw ProstoError{"TypeError", "unsupported operand type(s) for **: '" + typeStr() + "' and '" + v.typeStr() + "'"};
    }
}

Value Value::neg() const {
    switch (type) {
        case VT::Int: return Value(-i);
        case VT::Float: return Value(-f);
        default: throw ProstoError{"TypeError", "bad operand type for unary -: '" + typeStr() + "'"};
    }
}

Value Value::not_() const {
    return Value(!toBool());
}

Value Value::and_(const Value& v) const {
    return Value(toBool() && v.toBool());
}

Value Value::or_(const Value& v) const {
    return Value(toBool() || v.toBool());
}

Value Value::xor_(const Value& v) const {
    return Value(toBool() != v.toBool());
}

Value Value::eq(const Value& v) const {
    return Value(*this == v);
}

Value Value::ne(const Value& v) const {
    return Value(!equals(*this, v));
}

Value Value::lt(const Value& v) const {
    switch (type) {
        case VT::Int: return Value(i < v.toInt());
        case VT::Float: return Value(f < v.toFloat());
        case VT::String: return Value(s < v.toString());
        default: throw ProstoError{"TypeError", "unsupported operand type(s) for <: '" + typeStr() + "' and '" + v.typeStr() + "'"};
    }
}

Value Value::le(const Value& v) const {
    switch (type) {
        case VT::Int: return Value(i <= v.toInt());
        case VT::Float: return Value(f <= v.toFloat());
        case VT::String: return Value(s <= v.toString());
        default: throw ProstoError{"TypeError", "unsupported operand type(s) for <=: '" + typeStr() + "' and '" + v.typeStr() + "'"};
    }
}

Value Value::gt(const Value& v) const {
    switch (type) {
        case VT::Int: return Value(i > v.toInt());
        case VT::Float: return Value(f > v.toFloat());
        case VT::String: return Value(s > v.toString());
        default: throw ProstoError{"TypeError", "unsupported operand type(s) for >: '" + typeStr() + "' and '" + v.typeStr() + "'"};
    }
}

Value Value::ge(const Value& v) const {
    switch (type) {
        case VT::Int: return Value(i >= v.toInt());
        case VT::Float: return Value(f >= v.toFloat());
        case VT::String: return Value(s >= v.toString());
        default: throw ProstoError{"TypeError", "unsupported operand type(s) for >=: '" + typeStr() + "' and '" + v.typeStr() + "'"};
    }
}

std::string Value::typeStr() const {
    switch (type) {
        case VT::Null: return "null";
        case VT::Bool: return "bool";
        case VT::Int: return "int";
        case VT::Float: return "float";
        case VT::String: return "str";
        case VT::List: return "list";
        case VT::Dict: return "dict";
        case VT::Function: return "function";
        case VT::Class: return "class";
        case VT::Object: return "object";
        case VT::NativeObject: return "native object";
        case VT::Handle: return "handle";
    }
    return "";
}


long long Value::toInt() const {
    switch (type) {
        case VT::Int: return i;
        case VT::Float: return static_cast<long long>(f);
        case VT::Bool: return b ? 1 : 0;
        case VT::String: try { return std::stoll(s); } catch (...) { return 0; }
        default: return 0;
    }
}

double Value::toFloat() const {
    switch (type) {
        case VT::Float: return f;
        case VT::Int: return static_cast<double>(i);
        case VT::Bool: return b ? 1.0 : 0.0;
        case VT::String: try { return std::stod(s); } catch (...) { return 0.0; }
        default: return 0.0;
    }
}

std::string Value::toString() const {
    switch (type) {
        case VT::Null: return "None";
        case VT::Bool: return b ? "True" : "False";
        case VT::Int: return std::to_string(i);
        case VT::Float: {
            std::ostringstream oss;
            oss << f;
            return oss.str();
        }
        case VT::String: return s;
        default: return repr();
    }
}

std::string Value::toStr() const {
    return toString();
}

std::string Value::repr() const {
    switch (type) {
        case VT::Null: return "None";
        case VT::Bool: return b ? "True" : "False";
        case VT::Int: return std::to_string(i);
        case VT::Float: return std::to_string(f);
        case VT::String: return "\"" + s + "\"";
        case VT::List: {
            std::string out = "[";
            if (list) {
                for (size_t n = 0; n < list->size(); n++) {
                    if (n) out += ", ";
                    out += (*list)[n].repr();
                }
            }
            return out + "]";
        }
        case VT::Dict: {
            std::string out = "{";
            if (dict) {
                for (size_t n = 0; n < dict->items.size(); n++) {
                    if (n) out += ", ";
                    out += dict->items[n].first.repr() + ": " + dict->items[n].second.repr();
                }
            }
            return out + "}";
        }
        case VT::Function: return "<function " + (func ? func->name : "?") + ">";
        case VT::Class: return "<class " + (cls ? cls->name : "?") + ">";
        case VT::Object: return "<object>";
        case VT::NativeObject: return nat ? nat->repr() : "<native>";
        case VT::Handle: return "<handle:" + handleType + ">";
    }
    return "?";
}

std::string Value::keyString() const {
    if (isString()) return s;
    if (type == VT::Int) return std::to_string(i);
    return toString();
}

bool Value::equals(const Value& a, const Value& b) {
    if (a.type != b.type) {
        if (a.isNumber() && b.isNumber()) return a.toFloat() == b.toFloat();
        return false;
    }
    switch (a.type) {
        case VT::Null: return true;
        case VT::Bool: return a.b == b.b;
        case VT::Int: return a.i == b.i;
        case VT::Float: return a.f == b.f;
        case VT::String: return a.s == b.s;
        case VT::List: {
            if (!a.list || !b.list || a.list->size() != b.list->size()) return false;
            for (size_t n = 0; n < a.list->size(); n++)
                if (!equals((*a.list)[n], (*b.list)[n])) return false;
            return true;
        }
        case VT::Dict: {
            if (!a.dict || !b.dict || a.dict->items.size() != b.dict->items.size()) return false;
            for (auto& kv : a.dict->items)
                if (!equals(kv.second, b.dict->get(kv.first, Value()))) return false;
            return true;
        }
        default: return false;
    }
}

bool Value::operator==(const Value& other) const { return equals(*this, other); }

bool Value::operator<(const Value& other) const {
    if (isNumber() && other.isNumber()) return toFloat() < other.toFloat();
    if (isString() && other.isString()) return s < other.s;
    return static_cast<int>(type) < static_cast<int>(other.type);
}

// ---------------------------------------------------------------------------
// Dict
// ---------------------------------------------------------------------------
bool Dict::contains(const Value& k) const { return find(k) != nullptr; }

Value* Dict::find(const Value& k) {
    for (auto& kv : items)
        if (Value::equals(kv.first, k)) return &kv.second;
    return nullptr;
}

const Value* Dict::find(const Value& k) const {
    for (auto& kv : items)
        if (Value::equals(kv.first, k)) return &kv.second;
    return nullptr;
}

Value Dict::get(const Value& k, Value def) const {
    if (auto p = find(k)) return *p;
    return def;
}

void Dict::set(const Value& k, Value v) {
    if (auto p = find(k)) { *p = std::move(v); return; }
    items.push_back({k, std::move(v)});
}

bool Dict::erase(const Value& k) {
    for (size_t i = 0; i < items.size(); i++) {
        if (Value::equals(items[i].first, k)) {
            items.erase(items.begin() + static_cast<std::ptrdiff_t>(i));
            return true;
        }
    }
    return false;
}

Value makeListFromVector(std::vector<Value> v) {
    return Value::makeList(std::make_shared<ValueList>(std::move(v)));
}

bool lessValue(const Value& a, const Value& b) { return a < b; }

bool isDunder(const std::string& s) {
    return s.size() >= 4 && s.compare(0, 2, "__") == 0 &&
           s.compare(s.size() - 2, 2, "__") == 0;
}

// ---------------------------------------------------------------------------
// String / file utilities
// ---------------------------------------------------------------------------
std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) a++;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) b--;
    return s.substr(a, b - a);
}

bool startsWith(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}

bool endsWith(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0;
}

std::string upperString(const std::string& s) {
    std::string out = s;
    for (auto& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

std::string lowerString(const std::string& s) {
    std::string out = s;
    for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::vector<std::string> splitString(const std::string& s, const std::string& sep) {
    std::vector<std::string> out;
    if (sep.empty()) {
        std::istringstream iss(s);
        std::string tok;
        while (iss >> tok) out.push_back(tok);
        return out;
    }
    size_t start = 0;
    while (true) {
        size_t pos = s.find(sep, start);
        if (pos == std::string::npos) { out.push_back(s.substr(start)); break; }
        out.push_back(s.substr(start, pos - start));
        start = pos + sep.size();
    }
    return out;
}

std::vector<std::string> splitLines(const std::string& s) {
    std::vector<std::string> lines;
    std::string cur;
    for (char c : s) {
        if (c == '\n') { lines.push_back(cur); cur.clear(); }
        else if (c != '\r') cur += c;
    }
    if (!cur.empty() || !s.empty()) lines.push_back(cur);
    return lines;
}

std::string readFileAll(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw ProstoError{"FileNotFoundError", "file not found: " + path};
    std::ostringstream oss;
    oss << f.rdbuf();
    return oss.str();
}

void writeFileAll(const std::string& path, const std::string& text, bool append) {
    fs::path p(path);
    if (p.has_parent_path()) fs::create_directories(p.parent_path());
    std::ofstream f(path, append ? std::ios::app : std::ios::trunc);
    if (!f) throw ProstoError{"OSError", "cannot open file: " + path};
    f << text;
}

std::string generateUUID() {
    std::uniform_int_distribution<int> dist(0, 15);
    std::uniform_int_distribution<int> dist2(8, 11);
    const char* hex = "0123456789abcdef";
    std::string u;
    for (int i = 0; i < 36; i++) {
        if (i == 8 || i == 13 || i == 18 || i == 23) u += '-';
        else if (i == 14) u += '4';
        else if (i == 19) u += hex[dist2(rng())];
        else u += hex[dist(rng())];
    }
    return u;
}

std::string formatDate(const std::string& fmt) {
    time_t t = time(nullptr);
    tm tmv{};
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    char buf[512];
    strftime(buf, sizeof(buf), fmt.c_str(), &tmv);
    return std::string(buf);
}

std::string platformName() {
#ifdef _WIN32
    return "Windows";
#elif defined(__APPLE__)
    return "Darwin";
#else
    return "Linux";
#endif
}

std::string fileMtimeString(const std::string& path) {
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0) return "";
    time_t t = st.st_mtime;
    tm tmv{};
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmv);
    return std::string(buf);
}

// ---------------------------------------------------------------------------
// Crypto / encoding
// ---------------------------------------------------------------------------
std::string hexEncodeString(const std::string& s) {
    return hexEncode(reinterpret_cast<const unsigned char*>(s.data()), s.size());
}

std::string hexDecodeString(const std::string& s) {
    std::string out;
    for (size_t i = 0; i + 1 < s.size(); i += 2) {
        auto nibble = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return 0;
        };
        out += static_cast<char>((nibble(s[i]) << 4) | nibble(s[i + 1]));
    }
    return out;
}

std::string base64EncodeImpl(const unsigned char* data, size_t len, bool url) {
    std::string tbl = url
        ? "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"
        : "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    int val = 0, valb = -6;
    for (size_t i = 0; i < len; i++) {
        val = (val << 8) + data[i];
        valb += 8;
        while (valb >= 0) {
            out.push_back(tbl[(val >> valb) & 0x3F]);
            valb -= 6;
        }
    }
    if (valb > -6) out.push_back(tbl[(val << ((-valb) & 6)) & 0x3F]);
    if (!url) while (out.size() % 4) out.push_back('=');
    return out;
}

std::string base64DecodeImpl(const std::string& in, bool url) {
    std::vector<int> T(256, -1);
    std::string tbl = url
        ? "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"
        : "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (int i = 0; i < 64; i++) T[static_cast<unsigned char>(tbl[i])] = i;
    std::string out;
    int val = 0, valb = -8;
    for (unsigned char c : in) {
        if (T[c] == -1) break;
        val = (val << 6) + T[c];
        valb += 6;
        if (valb >= 0) {
            out.push_back(static_cast<char>((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return out;
}

std::string urlEncodeString(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += static_cast<char>(c);
        else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
    }
    return out;
}

std::string urlDecodeString(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size()) {
            auto nibble = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return 0;
            };
            out += static_cast<char>((nibble(s[i + 1]) << 4) | nibble(s[i + 2]));
            i += 2;
        } else if (s[i] == '+') out += ' ';
        else out += s[i];
    }
    return out;
}

std::string htmlEscapeString(const std::string& s) {
    std::string out;
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&#39;"; break;
            default: out += c;
        }
    }
    return out;
}

std::string htmlUnescapeString(const std::string& s) {
    std::string out = s;
    static const std::vector<std::pair<std::string, std::string>> reps = {
        {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&#39;", "'"},
        {"&apos;", "'"}, {"&amp;", "&"}
    };
    for (auto& r : reps) {
        size_t pos = 0;
        while ((pos = out.find(r.first, pos)) != std::string::npos) {
            out.replace(pos, r.first.size(), r.second);
            pos += r.second.size();
        }
    }
    return out;
}

uint32_t crc32String(const std::string& s) {
    uint32_t crc = 0xFFFFFFFF;
    for (unsigned char c : s) {
        crc ^= c;
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & static_cast<uint32_t>(-(static_cast<int32_t>(crc & 1))));
    }
    return crc ^ 0xFFFFFFFFu;
}

#ifdef _WIN32
static std::string toUtf8(const std::wstring& s);
static std::string bcryptDigestHex(const std::wstring& algorithm,
                                   const unsigned char* data,
                                   size_t len,
                                   const std::string& key = {}) {
    BCRYPT_ALG_HANDLE algoHandle = nullptr;
    DWORD flags = 0;
    if (!key.empty()) flags = BCRYPT_ALG_HANDLE_HMAC_FLAG;
    NTSTATUS status = BCryptOpenAlgorithmProvider(&algoHandle, algorithm.c_str(), nullptr, flags);
    if (!BCRYPT_SUCCESS(status)) {
        throw ProstoError{"ValueError", "unsupported digest: " + toUtf8(algorithm)};
    }

    DWORD objectSize = 0;
    DWORD resultSize = 0;
    status = BCryptGetProperty(algoHandle, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectSize), sizeof(objectSize), &resultSize, 0);
    if (!BCRYPT_SUCCESS(status)) {
        BCryptCloseAlgorithmProvider(algoHandle, 0);
        throw ProstoError{"RuntimeError", "bcrypt object length query failed"};
    }

    std::vector<unsigned char> objectBuffer(objectSize);
    BCRYPT_HASH_HANDLE hashHandle = nullptr;
    status = BCryptCreateHash(algoHandle,
                              &hashHandle,
                              objectBuffer.data(),
                              objectBuffer.size(),
                              key.empty() ? nullptr : reinterpret_cast<PUCHAR>(const_cast<char*>(key.data())),
                              static_cast<ULONG>(key.size()),
                              0);
    if (!BCRYPT_SUCCESS(status)) {
        BCryptCloseAlgorithmProvider(algoHandle, 0);
        throw ProstoError{"RuntimeError", "bcrypt hash creation failed"};
    }

    status = BCryptHashData(hashHandle, const_cast<PUCHAR>(data), static_cast<ULONG>(len), 0);
    if (!BCRYPT_SUCCESS(status)) {
        BCryptDestroyHash(hashHandle);
        BCryptCloseAlgorithmProvider(algoHandle, 0);
        throw ProstoError{"RuntimeError", "bcrypt hash update failed"};
    }

    DWORD hashSize = 0;
    status = BCryptGetProperty(algoHandle, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hashSize), sizeof(hashSize), &resultSize, 0);
    if (!BCRYPT_SUCCESS(status)) {
        BCryptDestroyHash(hashHandle);
        BCryptCloseAlgorithmProvider(algoHandle, 0);
        throw ProstoError{"RuntimeError", "bcrypt hash length query failed"};
    }

    std::vector<unsigned char> digest(hashSize);
    status = BCryptFinishHash(hashHandle, digest.data(), digest.size(), 0);
    BCryptDestroyHash(hashHandle);
    BCryptCloseAlgorithmProvider(algoHandle, 0);
    if (!BCRYPT_SUCCESS(status)) {
        throw ProstoError{"RuntimeError", "bcrypt hash finalization failed"};
    }

    return hexEncode(digest.data(), digest.size());
}

std::string evpDigestHex(const std::string& algo,
                         const unsigned char* data,
                         size_t len) {
    std::wstring algorithm;
    if (algo == "sha256" || algo == "SHA256") algorithm = BCRYPT_SHA256_ALGORITHM;
    else if (algo == "sha1" || algo == "SHA1") algorithm = BCRYPT_SHA1_ALGORITHM;
    else if (algo == "md5" || algo == "MD5") algorithm = BCRYPT_MD5_ALGORITHM;
    else if (algo == "sha512" || algo == "SHA512") algorithm = BCRYPT_SHA512_ALGORITHM;
    else throw ProstoError{"ValueError", "unsupported digest: " + algo};
    return bcryptDigestHex(algorithm, data, len);
}

std::string evpFileDigestHex(const std::string& path, const std::string& algo) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw ProstoError{"FileNotFoundError", "file not found: " + path};
    std::vector<unsigned char> buffer(8192);
    std::vector<unsigned char> data;
    while (f) {
        f.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
        std::streamsize read = f.gcount();
        if (read > 0) data.insert(data.end(), buffer.begin(), buffer.begin() + read);
    }
    return evpDigestHex(algo, data.empty() ? nullptr : data.data(), data.size());
}

std::string hmacSha256Hex(const std::string& key, const std::string& msg) {
    return bcryptDigestHex(BCRYPT_SHA256_ALGORITHM,
                           reinterpret_cast<const unsigned char*>(msg.data()),
                           msg.size(),
                           key);
}
#else
std::string evpDigestHex(const std::string& algo, const unsigned char* data, size_t len) {
    const EVP_MD* md = EVP_get_digestbyname(algo.c_str());
    if (!md) throw ProstoError{"ValueError", "unsupported digest: " + algo};
    EvpMdCtx ctx;
    unsigned char out[EVP_MAX_MD_SIZE];
    unsigned int outlen = 0;
    EVP_DigestInit_ex(ctx.get(), md, nullptr);
    EVP_DigestUpdate(ctx.get(), data, len);
    EVP_DigestFinal_ex(ctx.get(), out, &outlen);
    return hexEncode(out, outlen);
}

std::string evpFileDigestHex(const std::string& path, const std::string& algo) {
    const EVP_MD* md = EVP_get_digestbyname(algo.c_str());
    if (!md) throw ProstoError{"ValueError", "unsupported digest: " + algo};
    std::ifstream f(path, std::ios::binary);
    if (!f) throw ProstoError{"FileNotFoundError", "file not found: " + path};
    EvpMdCtx ctx;
    unsigned char out[EVP_MAX_MD_SIZE];
    unsigned int outlen = 0;
    EVP_DigestInit_ex(ctx.get(), md, nullptr);
    char buf[8192];
    while (f.read(buf, sizeof(buf)) || f.gcount()) {
        EVP_DigestUpdate(ctx.get(), buf, static_cast<size_t>(f.gcount()));
        if (f.eof()) break;
    }
    EVP_DigestFinal_ex(ctx.get(), out, &outlen);
    return hexEncode(out, outlen);
}

std::string hmacSha256Hex(const std::string& key, const std::string& msg) {
    unsigned char out[EVP_MAX_MD_SIZE];
    unsigned int outlen = 0;
    HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
         reinterpret_cast<const unsigned char*>(msg.data()), msg.size(), out, &outlen);
    return hexEncode(out, outlen);
}
#endif

// ---------------------------------------------------------------------------
// JSON conversion
// ---------------------------------------------------------------------------
json valueToJson(const Value& v) {
    switch (v.type) {
        case VT::Null: return nullptr;
        case VT::Bool: return v.b;
        case VT::Int: return v.i;
        case VT::Float: return v.f;
        case VT::String: return v.s;
        case VT::List: {
            json arr = json::array();
            if (v.list) for (auto& x : *v.list) arr.push_back(valueToJson(x));
            return arr;
        }
        case VT::Dict: {
            json obj = json::object();
            if (v.dict) for (auto& kv : v.dict->items) obj[kv.first.keyString()] = valueToJson(kv.second);
            return obj;
        }
        default: return v.toString();
    }
}

Value jsonToValue(const json& j) {
    if (j.is_null()) return Value();
    if (j.is_boolean()) return Value(j.get<bool>());
    if (j.is_number_integer()) return Value(static_cast<long long>(j.get<int64_t>()));
    if (j.is_number_unsigned()) return Value(static_cast<long long>(j.get<uint64_t>()));
    if (j.is_number_float()) return Value(j.get<double>());
    if (j.is_string()) return Value(j.get<std::string>());
    if (j.is_array()) {
        std::vector<Value> arr;
        for (auto& x : j) arr.push_back(jsonToValue(x));
        return makeListFromVector(arr);
    }
    if (j.is_object()) {
        auto d = std::make_shared<Dict>();
        for (auto it = j.begin(); it != j.end(); ++it)
            d->set(Value(it.key()), jsonToValue(it.value()));
        return Value::makeDict(d);
    }
    return Value();
}

// ---------------------------------------------------------------------------
// Format / template
// ---------------------------------------------------------------------------
std::string formatString(const std::string& fmt, const std::vector<Value>& args) {
    std::string out;
    size_t autoIdx = 0;
    for (size_t i = 0; i < fmt.size(); i++) {
        if (fmt[i] == '{') {
            if (i + 1 < fmt.size() && fmt[i + 1] == '{') { out += '{'; i++; continue; }
            size_t end = fmt.find('}', i);
            if (end == std::string::npos) { out += fmt[i]; continue; }
            std::string key = trim(fmt.substr(i + 1, end - i - 1));
            Value val;
            if (key.empty()) { if (autoIdx < args.size()) val = args[autoIdx++]; }
            else if (std::all_of(key.begin(), key.end(), ::isdigit)) {
                size_t idx = std::stoul(key);
                if (idx < args.size()) val = args[idx];
            } else if (!args.empty() && args.back().isDict()) {
                val = args.back().dict->get(Value(key), Value("{" + key + "}"));
            } else val = Value("{" + key + "}");
            out += val.toString();
            i = end;
        } else if (fmt[i] == '}' && i + 1 < fmt.size() && fmt[i + 1] == '}') {
            out += '}'; i++;
        } else out += fmt[i];
    }
    return out;
}

std::string templateString(const std::string& tmpl, const Value& data) {
    std::string out;
    for (size_t i = 0; i < tmpl.size(); i++) {
        if (tmpl[i] == '$' && i + 1 < tmpl.size() && tmpl[i + 1] == '{') {
            size_t end = tmpl.find('}', i + 2);
            if (end == std::string::npos) { out += tmpl[i]; continue; }
            std::string key = tmpl.substr(i + 2, end - i - 2);
            out += data.isDict()
                ? data.dict->get(Value(key), Value("${" + key + "}")).toString()
                : "${" + key + "}";
            i = end;
        } else out += tmpl[i];
    }
    return out;
}

Value getKwargsDict(std::vector<Value>& args) {
    if (!args.empty() && args.back().kwargs && args.back().isDict()) {
        Value d = args.back();
        args.pop_back();
        return d;
    }
    return Value();
}

// ---------------------------------------------------------------------------
// Secure process execution (no shell string concatenation)
// ---------------------------------------------------------------------------
#ifdef _WIN32
static std::wstring toWide(const std::string& s) {
    if (s.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring ws(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, ws.data(), len);
    if (!ws.empty() && ws.back() == L'\0') ws.pop_back();
    return ws;
}

static std::string quoteArgWin(const std::string& arg) {
    if (arg.empty()) return "\"\"";
    if (arg.find_first_of(" \t\"") == std::string::npos) return arg;
    std::string out = "\"";
    size_t bs = 0;
    for (char c : arg) {
        if (c == '\\') { bs++; out += c; }
        else if (c == '"') { out += std::string(bs * 2 + 1, '\\'); out += '"'; bs = 0; }
        else { bs = 0; out += c; }
    }
    out += std::string(bs * 2, '\\') + "\"";
    return out;
}

int spawnProcess(const std::vector<std::string>& argv, const std::string& cwd,
                 const std::string& stdoutPath, const std::string& stderrPath) {
    if (argv.empty()) return -1;
    std::string cmdLine;
    for (size_t i = 0; i < argv.size(); i++) {
        if (i) cmdLine += ' ';
        cmdLine += quoteArgWin(argv[i]);
    }
    std::vector<char> cmdBuf(cmdLine.begin(), cmdLine.end());
    cmdBuf.push_back('\0');

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE hOut = INVALID_HANDLE_VALUE, hErr = INVALID_HANDLE_VALUE;
    if (!stdoutPath.empty()) {
        hOut = CreateFileA(stdoutPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    if (!stderrPath.empty()) {
        hErr = CreateFileA(stderrPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    }

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = hOut != INVALID_HANDLE_VALUE ? hOut : GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = hErr != INVALID_HANDLE_VALUE ? hErr : GetStdHandle(STD_ERROR_HANDLE);

    PROCESS_INFORMATION pi{};
    std::string cwdCopy = cwd;
    BOOL ok = CreateProcessA(
        nullptr, cmdBuf.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW, nullptr,
        cwd.empty() ? nullptr : cwdCopy.c_str(),
        &si, &pi);

    if (hOut != INVALID_HANDLE_VALUE) CloseHandle(hOut);
    if (hErr != INVALID_HANDLE_VALUE) CloseHandle(hErr);
    if (!ok) return -1;

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return static_cast<int>(code);
}

void openFileWithDefaultApp(const std::string& path) {
    // ShellExecute with file path only — no shell command injection
    ShellExecuteA(nullptr, "open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}
#else
int spawnProcess(const std::vector<std::string>& argv, const std::string& cwd,
                 const std::string& stdoutPath, const std::string& stderrPath) {
    if (argv.empty()) return -1;
    std::vector<char*> cargv;
    cargv.reserve(argv.size() + 1);
    for (auto& a : argv) cargv.push_back(const_cast<char*>(a.c_str()));
    cargv.push_back(nullptr);

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    int outFd = -1, errFd = -1;
    if (!stdoutPath.empty()) {
        outFd = open(stdoutPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (outFd >= 0) posix_spawn_file_actions_adddup2(&actions, outFd, STDOUT_FILENO);
    }
    if (!stderrPath.empty()) {
        errFd = open(stderrPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (errFd >= 0) posix_spawn_file_actions_adddup2(&actions, errFd, STDERR_FILENO);
    }

    pid_t pid = 0;
    int rc = posix_spawn(&pid, argv[0].c_str(), &actions, nullptr, cargv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (outFd >= 0) close(outFd);
    if (errFd >= 0) close(errFd);
    if (rc != 0) return -1;

    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

void openFileWithDefaultApp(const std::string& path) {
    spawnProcess({"xdg-open", path}, "", "", "");
}
#endif

// ---------------------------------------------------------------------------
// ZIP
// ---------------------------------------------------------------------------
bool zipFolder(const std::string& path, int level, bool removeAfter) {
    fs::path p = fs::absolute(path);
    if (!fs::exists(p)) return false;
    level = std::max(0, std::min(level, 9));
    std::string zipname = p.string() + ".zip";

#ifdef _WIN32
    std::string cmd = "Compress-Archive -Path \"" + p.string() + "\" -DestinationPath \"" + zipname + "\" -Force";
    std::vector<std::string> argv = {"powershell.exe", "-NoProfile", "-Command", cmd};
#else
    std::vector<std::string> argv = {"zip", "-r", zipname, p.string()};
#endif
    int code = spawnProcess(argv, p.parent_path().string(), "", "");
    if (code != 0) return false;
    if (removeAfter) { std::error_code ec; fs::remove_all(p, ec); }
    return fs::exists(zipname);
}

// ---------------------------------------------------------------------------
// HTTP / crypto support
// ---------------------------------------------------------------------------
#ifdef _WIN32

static std::string toUtf8(const std::wstring& s) {
    if (s.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string out(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), size, nullptr, nullptr);
    return out;
}

static std::string winHttpErrorMessage(DWORD err) {
    LPWSTR buf = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPWSTR)&buf, 0, nullptr);
    std::wstring ws = buf ? buf : L"Unknown error";
    if (buf) LocalFree(buf);
    return toUtf8(ws);
}

static bool parseUrl(const std::string& url, std::wstring& host, std::wstring& path, INTERNET_PORT& port, bool& secure) {
    URL_COMPONENTS comps;
    memset(&comps, 0, sizeof(comps));
    comps.dwStructSize = sizeof(comps);
    comps.dwSchemeLength = comps.dwHostNameLength = comps.dwUrlPathLength = comps.dwExtraInfoLength = 0;

    std::wstring wurl = toWide(url);
    if (!WinHttpCrackUrl(wurl.c_str(), (DWORD)wurl.size(), 0, &comps)) return false;
    host.assign(comps.lpszHostName, comps.dwHostNameLength);
    path.assign(comps.lpszUrlPath, comps.dwUrlPathLength);
    if (path.empty()) path = L"/";
    secure = comps.nScheme == INTERNET_SCHEME_HTTPS;
    port = comps.nPort;
    return true;
}

Value doHttpRequest(const std::string& method, const std::string& url,
                    const Value& headers, const Value& data, int timeout) {
    std::wstring host;
    std::wstring path;
    INTERNET_PORT port = INTERNET_DEFAULT_HTTPS_PORT;
    bool secure = false;
    if (!parseUrl(url, host, path, port, secure)) {
        throw ProstoError{"RuntimeError", "invalid URL: " + url};
    }

    std::vector<std::pair<std::string, std::string>> hdrs;
    bool hasContentType = false;
    if (headers.isDict()) {
        for (auto& kv : headers.dict->items) {
            std::string k = kv.first.toString();
            std::string v = kv.second.toString();
            if (lowerString(k) == "content-type") hasContentType = true;
            hdrs.emplace_back(k, v);
        }
    }

    std::string body;
    if (!data.isNull()) {
        if (data.isDict()) {
            if (!hasContentType) {
                body = valueToJson(data).dump();
                hdrs.emplace_back("Content-Type", "application/json");
            } else {
                std::string q;
                for (auto& kv : data.dict->items) {
                    if (!q.empty()) q += "&";
                    q += urlEncodeString(kv.first.toString()) + "=" + urlEncodeString(kv.second.toString());
                }
                body = q;
            }
        } else {
            body = data.toString();
        }
    }

    std::string headersText;
    for (auto& h : hdrs) headersText += h.first + ": " + h.second + "\r\n";
    std::wstring wMethod = toWide(method);

    WinHttpHandle session(WinHttpOpen(L"Prosto+", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) throw ProstoError{"RuntimeError", "WinHTTP session failed: " + winHttpErrorMessage(GetLastError())};
    WinHttpHandle connection(WinHttpConnect(session.get(), host.c_str(), port, 0));
    if (!connection) throw ProstoError{"RuntimeError", "WinHTTP connect failed: " + winHttpErrorMessage(GetLastError())};

    DWORD flags = secure ? WINHTTP_FLAG_SECURE : 0;
    WinHttpHandle request(WinHttpOpenRequest(connection.get(), wMethod.c_str(), path.c_str(), L"HTTP/1.1", WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags));
    if (!request) throw ProstoError{"RuntimeError", "WinHTTP open request failed: " + winHttpErrorMessage(GetLastError())};

    BOOL ok = WinHttpSendRequest(request.get(), headersText.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : toWide(headersText).c_str(), (DWORD)(headersText.empty() ? 0 : toWide(headersText).size()),
                                body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)body.data(), (DWORD)body.size(), (DWORD)body.size(), 0);
    if (!ok) throw ProstoError{"RuntimeError", "WinHTTP send request failed: " + winHttpErrorMessage(GetLastError())};

    if (!WinHttpReceiveResponse(request.get(), nullptr)) {
        throw ProstoError{"RuntimeError", "WinHTTP receive response failed: " + winHttpErrorMessage(GetLastError())};
    }

    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);
    WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX);

    DWORD headerLen = 0;
    WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &headerLen, WINHTTP_NO_HEADER_INDEX);
    std::wstring rawHeaders;
    if (headerLen > 0) {
        rawHeaders.resize(headerLen / sizeof(wchar_t));
        WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX,
                            rawHeaders.data(), &headerLen, WINHTTP_NO_HEADER_INDEX);
    }

    std::vector<std::pair<std::string, std::string>> responseHeaders;
    std::wstring::size_type start = 0;
    while (start < rawHeaders.size()) {
        auto end = rawHeaders.find(L"\r\n", start);
        if (end == std::wstring::npos) end = rawHeaders.size();
        std::wstring headerLine = rawHeaders.substr(start, end - start);
        auto colon = headerLine.find(L':');
        if (colon != std::wstring::npos) {
            std::wstring nameW(headerLine.begin(), headerLine.begin() + colon);
            std::wstring valueW(headerLine.begin() + colon + 1, headerLine.end());
            responseHeaders.emplace_back(trim(toUtf8(nameW)), trim(toUtf8(valueW)));
        }
        if (end == rawHeaders.size()) break;
        start = end + 2;
    }

    std::string responseBody;
    std::vector<char> buffer(4096);
    DWORD bytesRead = 0;
    while (WinHttpReadData(request.get(), buffer.data(), (DWORD)buffer.size(), &bytesRead) && bytesRead > 0) {
        responseBody.append(buffer.data(), bytesRead);
    }

    return Value::makeNativeObject(std::make_shared<HttpResponseNative>(static_cast<long>(statusCode), responseHeaders, responseBody));
}

Value makeHttpResponse(long status, const std::vector<std::pair<std::string, std::string>>& headers, const std::string& body) {
    return Value::makeNativeObject(std::make_shared<HttpResponseNative>(status, headers, body));
}

bool httpDownload(const std::string& url, const std::string& savePath, int timeout) {
    std::vector<std::pair<std::string, std::string>> headers;
    Value resp = doHttpRequest("GET", url, Value::makeDict(std::make_shared<Dict>()), Value(), timeout);
    auto native = resp.nat;
    auto httpResp = dynamic_cast<HttpResponseNative*>(native.get());
    if (!httpResp) return false;
    if (httpResp->status < 200 || httpResp->status >= 300) return false;
    std::ofstream f(savePath, std::ios::binary);
    if (!f) return false;
    f.write(httpResp->body.data(), static_cast<std::streamsize>(httpResp->body.size()));
    return f.good();
}

#else

static size_t curlWriteString(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* s = static_cast<std::string*>(userdata);
    s->append(ptr, size * nmemb);
    return size * nmemb;
}

static size_t curlWriteFile(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* f = static_cast<std::ofstream*>(userdata);
    f->write(ptr, static_cast<std::streamsize>(size * nmemb));
    return size * nmemb;
}

static size_t curlHeaderCallback(char* buffer, size_t size, size_t nitems, void* userdata) {
    auto* headers = static_cast<std::vector<std::pair<std::string, std::string>>*>(userdata);
    std::string line(buffer, size * nitems);
    size_t pos = line.find(':');
    if (pos != std::string::npos) {
        std::string k = trim(line.substr(0, pos));
        std::string v = trim(line.substr(pos + 1));
        if (!k.empty()) headers->push_back({k, v});
    }
    return size * nitems;
}

Value doHttpRequest(const std::string& method, const std::string& url,
                    const Value& headers, const Value& data, int timeout) {
    CurlEasy curl;
    if (!curl) throw ProstoError{"RuntimeError", "curl init failed"};

    std::string body;
    std::vector<std::pair<std::string, std::string>> hdrs;
    bool hasContentType = false;

    if (headers.isDict()) {
        for (auto& kv : headers.dict->items) {
            std::string k = kv.first.toString();
            std::string v = kv.second.toString();
            if (lowerString(k) == "content-type") hasContentType = true;
            hdrs.push_back({k, v});
        }
    }

    if (!data.isNull()) {
        if (data.isDict()) {
            if (!hasContentType) {
                body = valueToJson(data).dump();
                hdrs.push_back({"Content-Type", "application/json"});
            } else {
                std::string q;
                for (auto& kv : data.dict->items) {
                    if (!q.empty()) q += "&";
                    q += urlEncodeString(kv.first.toString()) + "=" + urlEncodeString(kv.second.toString());
                }
                body = q;
            }
        } else {
            body = data.toString();
        }
    }

    std::string responseBody;
    std::vector<std::pair<std::string, std::string>> responseHeaders;
    CurlSlist slist;
    for (auto& h : hdrs) slist.append(h.first + ": " + h.second);

    curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT_MS, static_cast<long>(timeout * 1000));
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, curlWriteString);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &responseBody);
    curl_easy_setopt(curl.get(), CURLOPT_HEADERFUNCTION, curlHeaderCallback);
    curl_easy_setopt(curl.get(), CURLOPT_HEADERDATA, &responseHeaders);
    curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYHOST, 2L);
    if (slist.get()) curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, slist.get());

    if (method == "POST") {
        curl_easy_setopt(curl.get(), CURLOPT_POST, 1L);
        curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    } else if (method != "GET") {
        curl_easy_setopt(curl.get(), CURLOPT_CUSTOMREQUEST, method.c_str());
        if (!body.empty()) {
            curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, body.c_str());
            curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
        }
    }

    CURLcode res = curl_easy_perform(curl.get());
    long status = 0;
    if (res == CURLE_OK) curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status);
    else responseBody = curl_easy_strerror(res);

    return Value::makeNativeObject(
        std::make_shared<HttpResponseNative>(status, responseHeaders, responseBody));
}

bool httpDownload(const std::string& url, const std::string& savePath, int timeout) {
    CurlEasy curl;
    if (!curl) return false;
    std::ofstream f(savePath, std::ios::binary);
    if (!f) return false;

    curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT_MS, static_cast<long>(timeout * 1000));
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, curlWriteFile);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &f);
    curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYHOST, 2L);
    return curl_easy_perform(curl.get()) == CURLE_OK;
}

#endif

// ---------------------------------------------------------------------------
// EFCObject — file handle wrapper
// ---------------------------------------------------------------------------
struct EFCObject : NativeObject, std::enable_shared_from_this<EFCObject> {
    fs::path path;
    bool exists_flag = false;
    bool is_file = false;
    bool is_dir = false;
    std::string content_cache;

    explicit EFCObject(const std::string& p) : path(p) { refresh(); }

    void refresh() {
        exists_flag = fs::exists(path);
        is_file = exists_flag && fs::is_regular_file(path);
        is_dir = exists_flag && fs::is_directory(path);
        content_cache.clear();
    }

    void write(const std::string& text) {
        writeFileAll(path.string(), text, false);
        refresh();
    }

    void append(const std::string& text) {
        writeFileAll(path.string(), text, true);
        refresh();
    }

    long long size() const {
        if (!is_file) return 0;
        std::error_code ec;
        return static_cast<long long>(fs::file_size(path, ec));
    }

    std::string mtime() const { return fileMtimeString(path.string()); }

    void deleteSelf() {
        std::error_code ec;
        if (is_file) fs::remove(path, ec);
        else if (is_dir) fs::remove_all(path, ec);
        refresh();
    }

    void renameTo(const std::string& newname) {
        fs::rename(path, newname);
        path = newname;
        refresh();
    }

    void copyTo(const std::string& targetDir) {
        fs::copy(path, fs::path(targetDir) / path.filename(),
                 fs::copy_options::overwrite_existing);
    }

    void moveTo(const std::string& targetDir) {
        fs::rename(path, fs::path(targetDir) / path.filename());
        path = fs::path(targetDir) / path.filename();
        refresh();
    }

    Value getAttr(Interpreter&, const std::string& name) override {
        if (name == "path") return Value(path.string());
        if (name == "exists") return Value(exists_flag);
        if (name == "is_file") return Value(is_file);
        if (name == "is_dir") return Value(is_dir);
        if (name == "size") return Value(size());
        if (name == "mtime") return Value(mtime());
        if (name == "content") {
            if (content_cache.empty() && is_file) content_cache = readFileAll(path.string());
            return Value(content_cache);
        }
        if (name == "write") return Value::makeNativeFunction([self = shared_from_this()](Interpreter&, std::vector<Value>& args) {
            if (!args.empty()) self->write(args[0].toString());
            return Value();
        }, "write");
        if (name == "append") return Value::makeNativeFunction([self = shared_from_this()](Interpreter&, std::vector<Value>& args) {
            if (!args.empty()) self->append(args[0].toString());
            return Value();
        }, "append");
        if (name == "delete") return Value::makeNativeFunction([self = shared_from_this()](Interpreter&, std::vector<Value>&) {
            self->deleteSelf(); return Value();
        }, "delete");
        if (name == "rename") return Value::makeNativeFunction([self = shared_from_this()](Interpreter&, std::vector<Value>& args) {
            if (!args.empty()) self->renameTo(args[0].toString());
            return Value();
        }, "rename");
        if (name == "copy") return Value::makeNativeFunction([self = shared_from_this()](Interpreter&, std::vector<Value>& args) {
            if (!args.empty()) self->copyTo(args[0].toString());
            return Value();
        }, "copy");
        if (name == "move") return Value::makeNativeFunction([self = shared_from_this()](Interpreter&, std::vector<Value>& args) {
            if (!args.empty()) self->moveTo(args[0].toString());
            return Value();
        }, "move");
        if (name == "refresh") return Value::makeNativeFunction([self = shared_from_this()](Interpreter&, std::vector<Value>&) {
            self->refresh(); return Value();
        }, "refresh");
        throw ProstoError{"AttributeError", "EFCObject has no attribute '" + name + "'"};
    }

    std::string repr() const override { return "<EFCObject " + path.string() + ">"; }
};

Value makeEFCObject(const std::string& path) {
    return Value::makeNativeObject(std::make_shared<EFCObject>(path));
}

Value searchFiles(const std::string& pattern, const Value& recursive) {
    fs::path p = fs::absolute(pattern);
    std::vector<Value> out;
    if (!fs::exists(p)) return makeListFromVector(out);

    int maxDepth = 0;
    bool rec = false;
    if (recursive.type == VT::Bool) { rec = recursive.b; maxDepth = rec ? 9999 : 0; }
    else if (recursive.type == VT::Int) { rec = true; maxDepth = static_cast<int>(recursive.i); }

    if (!rec) {
        out.push_back(Value::makeNativeObject(std::make_shared<EFCObject>(p.string())));
        return makeListFromVector(out);
    }

    std::function<void(const fs::path&, int)> walk = [&](const fs::path& fp, int depth) {
        if (depth > maxDepth) return;
        if (fs::is_regular_file(fp)) {
            out.push_back(Value::makeNativeObject(std::make_shared<EFCObject>(fp.string())));
            return;
        }
        if (fs::is_directory(fp)) {
            for (auto& e : fs::directory_iterator(fp)) {
                if (fs::is_directory(e.path())) {
                    if (depth + 1 <= maxDepth) walk(e.path(), depth + 1);
                } else {
                    out.push_back(Value::makeNativeObject(std::make_shared<EFCObject>(e.path().string())));
                }
            }
        }
    };

    if (fs::is_directory(p)) walk(p, 1);
    else out.push_back(Value::makeNativeObject(std::make_shared<EFCObject>(p.string())));
    return makeListFromVector(out);
}

std::string globToRegex(const std::string& pat) {
    std::string out = "^";
    for (char c : pat) {
        if (c == '*') out += ".*";
        else if (c == '?') out += ".";
        else if (strchr(".^$+()[]{}|\\", c)) { out += "\\"; out += c; }
        else out += c;
    }
    return out + "$";
}

std::vector<std::string> globFiles(const std::string& pattern) {
    fs::path p(pattern);
    fs::path dir = p.parent_path();
    std::string file = p.filename().string();
    if (dir.empty()) dir = ".";
    std::vector<std::string> out;
    if (!fs::exists(dir) || !fs::is_directory(dir)) return out;
    std::regex re(globToRegex(file));
    for (auto& e : fs::directory_iterator(dir)) {
        std::string name = e.path().filename().string();
        if (std::regex_match(name, re)) out.push_back(e.path().generic_string());
    }
    std::sort(out.begin(), out.end());
    return out;
}

// ---------------------------------------------------------------------------
// Binary operators (shared with interpreter)
// ---------------------------------------------------------------------------
Value applyBinary(const std::string& op, const Value& a, const Value& b) {
    if (op == "+") {
        if (a.isNumber() && b.isNumber()) {
            if (a.type == VT::Int && b.type == VT::Int) return Value(a.i + b.i);
            return Value(a.toFloat() + b.toFloat());
        }
        if (a.isString() && b.isString()) return Value(a.s + b.s);
        if (a.isList() && b.isList()) {
            auto out = std::make_shared<ValueList>(*a.list);
            for (auto& x : *b.list) out->push_back(x);
            return Value::makeList(out);
        }
        if (a.isDict() && b.isDict()) {
            auto out = std::make_shared<Dict>(*a.dict);
            for (auto& kv : b.dict->items) out->set(kv.first, kv.second);
            return Value::makeDict(out);
        }
        throw ProstoError{"TypeError", "unsupported operand types for +"};
    }
    if (op == "-") {
        if (a.isNumber() && b.isNumber()) {
            if (a.type == VT::Int && b.type == VT::Int) return Value(a.i - b.i);
            return Value(a.toFloat() - b.toFloat());
        }
        throw ProstoError{"TypeError", "unsupported operand types for -"};
    }
    if (op == "*") {
        if (a.isNumber() && b.isNumber()) {
            if (a.type == VT::Int && b.type == VT::Int) return Value(a.i * b.i);
            return Value(a.toFloat() * b.toFloat());
        }
        if (a.isString() && b.type == VT::Int) {
            std::string out;
            for (long long n = 0; n < b.i; n++) out += a.s;
            return Value(out);
        }
        if (a.isList() && b.type == VT::Int) {
            auto out = std::make_shared<ValueList>();
            for (long long n = 0; n < b.i; n++)
                for (auto& x : *a.list) out->push_back(x);
            return Value::makeList(out);
        }
        throw ProstoError{"TypeError", "unsupported operand types for *"};
    }
    if (op == "/") {
        if (a.isNumber() && b.isNumber()) {
            double rb = b.toFloat();
            if (rb == 0.0) throw ProstoError{"ZeroDivisionError", "division by zero"};
            return Value(a.toFloat() / rb);
        }
        throw ProstoError{"TypeError", "unsupported operand types for /"};
    }
    if (op == "//") {
        if (a.isNumber() && b.isNumber()) {
            if (a.type == VT::Int && b.type == VT::Int) {
                if (b.i == 0) throw ProstoError{"ZeroDivisionError", "division by zero"};
                long long q = a.i / b.i, r = a.i % b.i;
                if (r != 0 && ((r < 0) != (b.i < 0))) q--;
                return Value(q);
            }
            double rb = b.toFloat();
            if (rb == 0.0) throw ProstoError{"ZeroDivisionError", "division by zero"};
            return Value(std::floor(a.toFloat() / rb));
        }
        throw ProstoError{"TypeError", "unsupported operand types for //"};
    }
    if (op == "%") {
        if (a.isNumber() && b.isNumber()) {
            if (a.type == VT::Int && b.type == VT::Int) {
                if (b.i == 0) throw ProstoError{"ZeroDivisionError", "modulo by zero"};
                long long r = a.i % b.i;
                if (r != 0 && ((r < 0) != (b.i < 0))) r += b.i;
                return Value(r);
            }
            double rb = b.toFloat();
            if (rb == 0.0) throw ProstoError{"ZeroDivisionError", "modulo by zero"};
            double r = std::fmod(a.toFloat(), rb);
            if (r != 0 && ((r < 0) != (rb < 0))) r += rb;
            return Value(r);
        }
        throw ProstoError{"TypeError", "unsupported operand types for %"};
    }
    if (op == "**") {
        if (a.type == VT::Int && b.type == VT::Int && b.i >= 0) {
            long long res = 1;
            for (long long n = 0; n < b.i; n++) res *= a.i;
            return Value(res);
        }
        return Value(std::pow(a.toFloat(), b.toFloat()));
    }
    if (op == "&" || op == "|" || op == "^" || op == "<<" || op == ">>") {
        long long x = a.toInt(), y = b.toInt();
        if (op == "&") return Value(x & y);
        if (op == "|") return Value(x | y);
        if (op == "^") return Value(x ^ y);
        if (op == "<<") return Value(x << y);
        if (op == ">>") return Value(x >> y);
    }
    throw ProstoError{"RuntimeError", "unknown operator: " + op};
}

// Expose RNG for builtins
std::mt19937_64& globalRng() { return rng(); }

} // namespace prosto