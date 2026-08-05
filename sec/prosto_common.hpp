#pragma once

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <set>
#include <memory>
#include <functional>
#include <filesystem>
#include <thread>
#include <mutex>
#include <regex>
#include <random>
#include <chrono>
#include <ctime>
#include <cmath>
#include <algorithm>
#include <numeric>
#include <optional>
#include <any>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <iomanip>
#include <sys/stat.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <sys/wait.h>
#endif

#include <nlohmann/json.hpp>
#include <sqlite3.h>
#include <zip.h>
#include <curl/curl.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#ifndef M_E
#define M_E 2.71828182845904523536
#endif

using namespace std;

using std::string;
using json = nlohmann::json;
namespace fs = std::filesystem;

struct Interpreter;
struct Value;

using ValueList = vector<Value>;
using NativeFn = function<Value(Interpreter&, vector<Value>&)>;

// Value 相关函数声明

enum class VT {
    Null, Bool, Int, Float, String, List, Dict, Function, Class, Object, NativeObject, Handle
};

struct Dict {
    vector<pair<Value, Value>> items;

    bool contains(const Value& k) const;
    Value* find(const Value& k);
    const Value* find(const Value& k) const;
    Value get(const Value& k, Value def) const;
    void set(const Value& k, Value v);
    bool erase(const Value& k);
};

struct Function {
    string name = "<function>";
    vector<string> params;
    vector<string> block;
    bool native = false;
    bool method = false;
    NativeFn nativeFn;
};

struct Class {
    string name;
    unordered_map<string, shared_ptr<Function>> methods;
};

struct Object {
    shared_ptr<Class> cls;
    shared_ptr<Dict> attrs = make_shared<Dict>();
};

struct NativeObject {
    virtual ~NativeObject() = default;
    virtual Value getAttr(Interpreter&, const string&) = 0;
    virtual void setAttr(Interpreter&, const string&, const Value&) {}
    virtual string repr() const = 0;
};


struct Value {
    VT type = VT::Null;
    bool b = false;
    long long i = 0;
    double f = 0.0;
    std::string s;
    shared_ptr<ValueList> list;
    shared_ptr<Dict> dict;
    shared_ptr<Function> func;
    shared_ptr<Class> cls;
    shared_ptr<Object> obj;
    shared_ptr<NativeObject> nat;
    std::string handleType;
    std::any handle;
    bool kwargs = false;

    Value() = default;
    Value(bool v) : type(VT::Bool), b(v) {}
    Value(int v) : type(VT::Int), i(v) {}
    Value(long long v) : type(VT::Int), i(v) {}
    Value(double v) : type(VT::Float), f(v) {}
    Value(const char* v) : type(VT::String), s(v) {}
    Value(const std::string& v) : type(VT::String), s(v) {}

    static Value null() { return Value(); }
    static Value boolean(bool v) { return Value(v); }
    static Value integer(long long v) { return Value(v); }
    static Value number(double v) { return Value(v); }
    static Value string(const std::string& v) { return Value(v); }

    static Value makeList(shared_ptr<ValueList> v) {
        Value x; x.type = VT::List; x.list = v; return x;
    }
    static Value makeDict(shared_ptr<Dict> v) {
        Value x; x.type = VT::Dict; x.dict = v; return x;
    }
    static Value makeFunction(shared_ptr<Function> v) {
        Value x; x.type = VT::Function; x.func = v; return x;
    }
    static Value makeClass(shared_ptr<Class> v) {
        Value x; x.type = VT::Class; x.cls = v; return x;
    }
    static Value makeObject(shared_ptr<Object> v) {
        Value x; x.type = VT::Object; x.obj = v; return x;
    }
    static Value makeNativeObject(shared_ptr<NativeObject> v) {
        Value x; x.type = VT::NativeObject; x.nat = v; return x;
    }
    static Value makeHandle(const std::string& ht, std::any h) {
        Value x; x.type = VT::Handle; x.handleType = ht; x.handle = h; return x;
    }
    static Value makeNativeFunction(NativeFn fn, const std::string& name = "<native>") {
        auto f = make_shared<Function>();
        f->native = true;
        f->name = name;
        f->nativeFn = fn;
        return makeFunction(f);
    }

    bool isNull() const { return type == VT::Null; }
    bool isString() const { return type == VT::String; }
    bool isList() const { return type == VT::List; }
    bool isDict() const { return type == VT::Dict; }
    bool isFunction() const { return type == VT::Function; }
    bool isClass() const { return type == VT::Class; }
    bool isObject() const { return type == VT::Object; }
    bool isNativeObject() const { return type == VT::NativeObject; }
    bool isHandle() const { return type == VT::Handle; }
    bool isNumber() const { return type == VT::Int || type == VT::Float || type == VT::Bool; }

    bool toBool() const;
    long long toInt() const;
    double toFloat() const;
    std::string toStr() const;
    std::string repr() const;
    std::string keyString() const;

    static bool equals(const Value& a, const Value& b);
    // 添加比较运算符重载声明
    bool operator<(const Value& other) const;
    bool operator==(const Value& other) const;
};

struct ProstoError {
    std::string type;
    std::string msg;
};

struct ReturnSignal {
    Value value;
};

struct BreakSignal {};
struct ContinueSignal {};

struct SecurityError {
    std::string msg;
};

struct Scope {
    std::unordered_map<std::string, Value> vars;
    std::set<std::string> globalNames;
    std::shared_ptr<Scope> parent;
};

static mt19937_64 RNG((uint64_t)chrono::steady_clock::now().time_since_epoch().count());

// 添加缺失的函数声明
string trim(const string& s);
bool startsWith(const string& s, const string& p);
bool endsWith(const string& s, const string& p);
string upperString(const string& s);
string lowerString(const string& s);
vector<string> splitString(const string& s, const string& sep);
vector<string> splitLines(const string& s);
string readFileAll(const string& path);
void writeFileAll(const string& path, const string& text, bool append = false);
string generateUUID();
string formatDate(const string& fmt);
string platformName();
int braceDelta(const string& line);
bool startsWithSkipKeyword(const string& s);

// helpers used across translation units

struct HttpResponseNative : NativeObject {
    long status = 0;
    std::vector<std::pair<string,string>> headers;
    string body;
    HttpResponseNative(long st, std::vector<std::pair<string,string>> hd, string bd);
    Value getAttr(Interpreter& in, const string& name) override;
    string repr() const override;
};

struct EFCObject : NativeObject, std::enable_shared_from_this<EFCObject> {
    fs::path path;
    bool exists_flag = false;
    bool is_file = false;
    bool is_dir = false;
    string content_cache;
    EFCObject(const string& p);
    void refresh();
    void write(const string& text);
    void append(const string& text);
    long long size() const;
    string mtime() const;
    void deleteSelf();
    void renameTo(const string& newname);
    void copyTo(const string& targetDir);
    void moveTo(const string& targetDir);
    Value getAttr(Interpreter& in, const string& name) override;
    string repr() const override;
};

inline string formatString(const string& fmt, const vector<Value>& args);
inline json valueToJson(const Value& v);
inline Value jsonToValue(const json& j);
inline Value makeListFromVector(vector<Value> v);
inline bool lessValue(const Value& a, const Value& b);
inline bool isDunder(const string& s);
inline Value applyBinary(const string& op, const Value& a, const Value& b);
inline Value searchFiles(const string& pattern, const Value& recursive);

// utility functions exposed across TU
inline Value doHttpRequest(const string& method, const string& url, const Value& headers, const Value& data, int timeout);
inline bool httpDownload(const string& url, const string& savePath, int timeout);
string hexEncodeString(const string& s);
string hexDecodeString(const string& s);
string base64EncodeImpl(const unsigned char* data, size_t len, bool url);
string base64DecodeImpl(const string& in, bool url);
string urlEncodeString(const string& s);
string urlDecodeString(const string& s);
string htmlEscapeString(const string& s);
string htmlUnescapeString(const string& s);
uint32_t crc32String(const string& s);
string evpDigestHex(const string& algo, const unsigned char* data, size_t len);
string evpFileDigestHex(const string& path, const string& algo);
string hmacSha256Hex(const string& key, const string& msg);
string fileMtimeString(const string& path);
vector<string> globFiles(const string& pattern);
string templateString(const string& tmpl, const Value& data);
Value getKwargsDict(vector<Value>& args);
void openWithSystem(const string& path);
bool zipFolder(const string& path, int level, bool removeAfter);


// 添加 Value 相关函数的声明

// 对外提供 Interpreter 的接口声明（实现位于 prosto_runtime.cpp）
struct Interpreter {
    unordered_map<string, Value> globals;
    unordered_map<string, shared_ptr<Function>> functions;
    unordered_map<string, shared_ptr<Class>> classes;
    unordered_map<string, Value> builtins;
    unordered_set<string> importedFiles;
    unordered_set<string> importedPackages;
    vector<pair<string, int>> callStack;
    recursive_mutex globalMutex;
    Interpreter();
    void registerBuiltins();

    Value getVar(const string& name, shared_ptr<Scope> scope);
    void assignVar(const string& name, Value val, shared_ptr<Scope> scope);

    Value evalExpr(const string& expr, shared_ptr<Scope> scope);
    void runLines(const vector<string>& lines, shared_ptr<Scope> scope, int baseLine = 1);

    Value callValue(Value callee, vector<Value>& args, shared_ptr<Scope> scope = nullptr);

    Value getAttr(const Value& obj, const string& name);
    void setAttr(Value& obj, const string& name, const Value& val);
    Value indexValue(const Value& obj, const Value& idx);
    Value sliceValue(const Value& obj, const Value& start, const Value& end, const Value& step);

    void importPackage(const string& name, shared_ptr<Scope> scope, int ln);
    void printError(const ProstoError& e, int ln);
};