#pragma once

#include <any>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace prosto {

// ---------------------------------------------------------------------------
// DLL export macro
// ---------------------------------------------------------------------------
#if defined(_WIN32) || defined(_WIN64)
#  ifdef PROSTO_BUILD_DLL
#    define PROSTO_API __declspec(dllexport)
#  else
#    define PROSTO_API __declspec(dllimport)
#  endif
#else
#  define PROSTO_API __attribute__((visibility("default")))
#endif

struct Interpreter;

enum class VT {
    Null, Bool, Int, Float, String, List, Dict, Function, Class, Object, NativeObject, Handle
};

using ValueList = std::vector<struct Value>;
using NativeFn = std::function<Value(Interpreter&, std::vector<Value>&)>;

struct Dict {
    std::vector<std::pair<Value, Value>> items;

    bool contains(const Value& k) const;
    Value* find(const Value& k);
    const Value* find(const Value& k) const;
    Value get(const Value& k, Value def) const;
    void set(const Value& k, Value v);
    bool erase(const Value& k);
};

struct Function {
    std::string name = "<function>";
    std::vector<std::string> params;
    std::vector<std::string> block;
    bool native = false;
    bool method = false;
    NativeFn nativeFn;
};

struct Class {
    std::string name;
    std::unordered_map<std::string, std::shared_ptr<Function>> methods;
};

struct Object {
    std::shared_ptr<Class> cls;
    std::shared_ptr<Dict> attrs = std::make_shared<Dict>();
};

struct NativeObject {
    virtual ~NativeObject() = default;
    virtual Value getAttr(Interpreter&, const std::string&) = 0;
    virtual void setAttr(Interpreter&, const std::string&, const Value&) {}
    virtual std::string repr() const = 0;
};

struct Value {
    VT type = VT::Null;
    bool b = false;
    long long i = 0;
    double f = 0.0;
    std::string s;
    std::shared_ptr<ValueList> list;
    std::shared_ptr<Dict> dict;
    std::shared_ptr<Function> func;
    std::shared_ptr<Class> cls;
    std::shared_ptr<Object> obj;
    std::shared_ptr<NativeObject> nat;
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
    static Value makeList(std::shared_ptr<ValueList> v);
    static Value makeDict(std::shared_ptr<Dict> v);
    static Value makeFunction(std::shared_ptr<Function> v);
    static Value makeClass(std::shared_ptr<Class> v);
    static Value makeObject(std::shared_ptr<Object> v);
    static Value makeNativeObject(std::shared_ptr<NativeObject> v);
    static Value makeHandle(const std::string& ht, std::any h);
    static Value makeNativeFunction(NativeFn fn, const std::string& name = "<native>");

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
    std::string toString() const;
    std::string toStr() const;
    std::string repr() const;
    std::string keyString() const;

    // Attribute/call/index helpers (implemented in utils.cpp)
    Value getAttr(Interpreter& interp, const std::string& name) const;
    void setAttr(Interpreter& interp, const std::string& name, const Value& v);
    Value call(Interpreter& interp, const std::vector<Value>& args) const;
    Value index(Interpreter& interp, const Value& idx) const;

    // Arithmetic / logical operations returning Value wrappers
    Value add(const Value& v) const;
    Value sub(const Value& v) const;
    Value mul(const Value& v) const;
    Value div(const Value& v) const;
    Value mod(const Value& v) const;
    Value pow(const Value& v) const;
    Value neg() const;
    Value not_() const;
    Value and_(const Value& v) const;
    Value or_(const Value& v) const;
    Value xor_(const Value& v) const;

    // Comparison operators returning Value (bool wrapped)
    Value eq(const Value& v) const;
    Value ne(const Value& v) const;
    Value lt(const Value& v) const;
    Value le(const Value& v) const;
    Value gt(const Value& v) const;
    Value ge(const Value& v) const;

    std::string typeStr() const;

    static bool equals(const Value& a, const Value& b);
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

// Cross-TU helpers (implemented in utils.cpp / interpreter.cpp)
Value makeListFromVector(std::vector<Value> v);
bool lessValue(const Value& a, const Value& b);
bool isDunder(const std::string& s);
Value applyBinary(const std::string& op, const Value& a, const Value& b);

} // namespace prosto
