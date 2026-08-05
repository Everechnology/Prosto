#pragma once

#include "prosto/types.hpp"

#include <string>
#include <vector>

namespace prosto {

class PROSTO_API Interpreter {
public:
    Interpreter();
    ~Interpreter();

    Interpreter(const Interpreter&) = delete;
    Interpreter& operator=(const Interpreter&) = delete;

    void registerBuiltins();

    Value getVar(const std::string& name, std::shared_ptr<Scope> scope);
    void assignVar(const std::string& name, Value val, std::shared_ptr<Scope> scope);

    Value evalExpr(const std::string& expr, std::shared_ptr<Scope> scope);
    void runLines(const std::vector<std::string>& lines, std::shared_ptr<Scope> scope, int baseLine = 1);
    void runFile(const std::string& path);

    Value callValue(Value callee, std::vector<Value>& args, std::shared_ptr<Scope> scope = nullptr);

    Value getAttr(const Value& obj, const std::string& name);
    void setAttr(Value& obj, const std::string& name, const Value& val);
    Value indexValue(const Value& obj, const Value& idx);
    Value sliceValue(const Value& obj, const Value& start, const Value& end, const Value& step);

    void importPackage(const std::string& name, std::shared_ptr<Scope> scope, int ln);
    void printError(const ProstoError& e, int ln);

    std::unordered_map<std::string, Value> globals() const;
    std::unordered_map<std::string, std::shared_ptr<Function>> functions() const;

private:

    std::unordered_map<std::string, Value> globals_;
    std::unordered_map<std::string, std::shared_ptr<Function>> functions_;
    std::unordered_map<std::string, std::shared_ptr<Class>> classes_;
    std::unordered_map<std::string, Value> builtins_;
    std::unordered_set<std::string> importedFiles_;
    std::unordered_set<std::string> importedPackages_;
    std::vector<std::pair<std::string, int>> callStack_;

    // Thread safety: all mutable interpreter state guarded by recursive mutex
    mutable std::recursive_mutex mutex_;

    void lock() const;
};

} // namespace prosto
