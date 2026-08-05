#include "prosto/interpreter.hpp"
#include "prosto/utils_decl.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <optional>
#include <regex>
#include <sstream>
#include <thread>

#include <nlohmann/json.hpp>

namespace prosto {

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

static bool compareValues(const Value& a, const std::string& op, const Value& b);
static bool valueIn(const Value& item, const Value& container);

enum class TokenType {
    End,
    Identifier,
    Number,
    String,
    Op,
    LParen,
    RParen,
    LBracket,
    RBracket,
    LBrace,
    RBrace,
    Comma,
    Colon,
    Dot,
};

struct Token {
    TokenType type;
    std::string text;
};

class ExprParserImpl {
public:
    ExprParserImpl(Interpreter& interp, const std::string& expr, std::shared_ptr<Scope> scope)
        : interp_(interp), text_(expr), scope_(std::move(scope)), pos_(0) {
        tokenize();
    }

    Value parse() {
        Value result = parseOr();
        if (current().type != TokenType::End) {
            throw ProstoError{"SyntaxError", "unexpected token: " + current().text};
        }
        return result;
    }

private:
    Interpreter& interp_;
    std::string text_;
    std::shared_ptr<Scope> scope_;
    size_t pos_;
    std::vector<Token> tokens_;

    const Token& current() const {
        if (pos_ >= tokens_.size()) return tokens_.back();
        return tokens_[pos_];
    }

    const Token& peek(size_t offset = 1) const {
        if (pos_ + offset >= tokens_.size()) return tokens_.back();
        return tokens_[pos_ + offset];
    }

    void advance() {
        if (pos_ < tokens_.size()) ++pos_;
    }

    bool accept(TokenType type, const std::string& text = "") {
        if (current().type == type && (text.empty() || current().text == text)) {
            advance();
            return true;
        }
        return false;
    }

    void consume(TokenType type, const std::string& text = "") {
        if (!accept(type, text)) {
            throw ProstoError{"SyntaxError", "expected " + tokenName(type) + (text.empty() ? "" : ": " + text)};
        }
    }

    static std::string tokenName(TokenType type) {
        switch (type) {
            case TokenType::End: return "end";
            case TokenType::Identifier: return "identifier";
            case TokenType::Number: return "number";
            case TokenType::String: return "string";
            case TokenType::Op: return "operator";
            case TokenType::LParen: return "(";
            case TokenType::RParen: return ")";
            case TokenType::LBracket: return "[";
            case TokenType::RBracket: return "]";
            case TokenType::LBrace: return "{";
            case TokenType::RBrace: return "}";
            case TokenType::Comma: return ",";
            case TokenType::Colon: return ":";
            case TokenType::Dot: return ".";
        }
        return "token";
    }

    void tokenize() {
        size_t i = 0;
        while (i < text_.size()) {
            char c = text_[i];
            if (std::isspace(static_cast<unsigned char>(c))) {
                i++;
                continue;
            }

            if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
                size_t start = i;
                i++;
                while (i < text_.size() && (std::isalnum(static_cast<unsigned char>(text_[i])) || text_[i] == '_')) {
                    i++;
                }
                tokens_.push_back({TokenType::Identifier, text_.substr(start, i - start)});
                continue;
            }

            if (std::isdigit(static_cast<unsigned char>(c)) || (c == '.' && i + 1 < text_.size() && std::isdigit(static_cast<unsigned char>(text_[i + 1])))) {
                size_t start = i;
                bool hasDot = false;
                if (c == '.') {
                    hasDot = true;
                    i++;
                }
                while (i < text_.size() && (std::isdigit(static_cast<unsigned char>(text_[i])) || (!hasDot && text_[i] == '.'))) {
                    if (text_[i] == '.') hasDot = true;
                    i++;
                }
                tokens_.push_back({TokenType::Number, text_.substr(start, i - start)});
                continue;
            }

            if (c == '"' || c == '\'') {
                char quote = c;
                i++;
                std::string value;
                while (i < text_.size() && text_[i] != quote) {
                    if (text_[i] == '\\' && i + 1 < text_.size()) {
                        i++;
                        value.push_back(text_[i]);
                    } else {
                        value.push_back(text_[i]);
                    }
                    i++;
                }
                if (i < text_.size() && text_[i] == quote) {
                    i++;
                }
                tokens_.push_back({TokenType::String, value});
                continue;
            }

            if (c == '(') { tokens_.push_back({TokenType::LParen, "("}); i++; continue; }
            if (c == ')') { tokens_.push_back({TokenType::RParen, ")"}); i++; continue; }
            if (c == '[') { tokens_.push_back({TokenType::LBracket, "["}); i++; continue; }
            if (c == ']') { tokens_.push_back({TokenType::RBracket, "]"}); i++; continue; }
            if (c == '{') { tokens_.push_back({TokenType::LBrace, "{"}); i++; continue; }
            if (c == '}') { tokens_.push_back({TokenType::RBrace, "}"}); i++; continue; }
            if (c == ',') { tokens_.push_back({TokenType::Comma, ","}); i++; continue; }
            if (c == ':') { tokens_.push_back({TokenType::Colon, ":"}); i++; continue; }
            if (c == '.') { tokens_.push_back({TokenType::Dot, "."}); i++; continue; }

            auto matchOp = [&](const std::string& op) {
                if (text_.substr(i, op.size()) == op) {
                    tokens_.push_back({TokenType::Op, op});
                    i += op.size();
                    return true;
                }
                return false;
            };

            if (matchOp("==") || matchOp("!=") || matchOp("<=") || matchOp(">=") ||
                matchOp("<<") || matchOp(">>") || matchOp("**") || matchOp("//")) {
                continue;
            }

            std::string singleOps = "+-*/%<>&|^=!";
            if (singleOps.find(c) != std::string::npos) {
                tokens_.push_back({TokenType::Op, std::string(1, c)});
                i++;
                continue;
            }

            throw ProstoError{"SyntaxError", std::string("invalid character: ") + c};
        }
        tokens_.push_back({TokenType::End, ""});
    }

    Value parseOr() {
        Value left = parseAnd();
        while (current().type == TokenType::Identifier && current().text == "or") {
            advance();
            Value right = parseAnd();
            if (left.toBool()) return left;
            left = right;
        }
        return left;
    }

    Value parseAnd() {
        Value left = parseNot();
        while (current().type == TokenType::Identifier && current().text == "and") {
            advance();
            Value right = parseNot();
            if (!left.toBool()) return left;
            left = right;
        }
        return left;
    }

    Value parseNot() {
        if (current().type == TokenType::Identifier && current().text == "not") {
            advance();
            Value value = parseNot();
            return Value(!value.toBool());
        }
        return parseComparison();
    }

    Value parseComparison() {
        Value left = parseBitwise();
        while (current().type == TokenType::Op || current().type == TokenType::Identifier) {
            std::string op = current().text;
            if (current().type == TokenType::Identifier && (op == "in" || op == "not")) {
                if (op == "not" && peek().type == TokenType::Identifier && peek().text == "in") {
                    advance();
                    advance();
                    Value right = parseBitwise();
                    return Value(!valueIn(left, right));
                }
                if (op == "in") {
                    advance();
                    Value right = parseBitwise();
                    return Value(valueIn(left, right));
                }
            }
            if (op == "==" || op == "!=" || op == "<" || op == ">" || op == "<=" || op == ">=") {
                advance();
                Value right = parseBitwise();
                return Value(compareValues(left, op, right));
            }
            break;
        }
        return left;
    }

    Value parseBitwise() {
        Value left = parseShift();
        while (current().type == TokenType::Op && (current().text == "&" || current().text == "|" || current().text == "^")) {
            std::string op = current().text;
            advance();
            Value right = parseShift();
            left = prosto::applyBinary(op, left, right);
        }
        return left;
    }

    Value parseShift() {
        Value left = parseTerm();
        while (current().type == TokenType::Op && (current().text == "<<" || current().text == ">>")) {
            std::string op = current().text;
            advance();
            Value right = parseTerm();
            left = prosto::applyBinary(op, left, right);
        }
        return left;
    }

    Value parseTerm() {
        Value left = parseFactor();
        while (current().type == TokenType::Op && (current().text == "+" || current().text == "-")) {
            std::string op = current().text;
            advance();
            Value right = parseFactor();
            left = prosto::applyBinary(op, left, right);
        }
        return left;
    }

    Value parseFactor() {
        Value left = parsePower();
        while (current().type == TokenType::Op && (current().text == "*" || current().text == "/" || current().text == "//" || current().text == "%")) {
            std::string op = current().text;
            advance();
            Value right = parsePower();
            left = prosto::applyBinary(op, left, right);
        }
        return left;
    }

    Value parsePower() {
        if (current().type == TokenType::Op && (current().text == "+" || current().text == "-")) {
            std::string op = current().text;
            advance();
            Value value = parsePower();
            if (op == "-") {
                if (value.type == VT::Int) return Value(-value.i);
                return Value(-value.toFloat());
            }
            return value;
        }
        Value left = parsePostfix();
        if (current().type == TokenType::Op && current().text == "**") {
            advance();
            Value right = parsePower();
            left = prosto::applyBinary("**", left, right);
        }
        return left;
    }

    Value parsePostfix() {
        Value value = parsePrimary();
        while (true) {
            if (accept(TokenType::Dot)) {
                if (current().type != TokenType::Identifier) {
                    throw ProstoError{"SyntaxError", "expected attribute name after '.'"};
                }
                std::string member = current().text;
                advance();
                value = interp_.getAttr(value, member);
                continue;
            }
            if (accept(TokenType::LParen)) {
                std::vector<Value> args;
                bool hasKwargs = false;
                std::shared_ptr<Dict> kwargs = std::make_shared<Dict>();
                while (!accept(TokenType::RParen)) {
                    if (current().type == TokenType::Identifier && peek().type == TokenType::Op && peek().text == "=") {
                        std::string name = current().text;
                        advance();
                        advance();
                        Value argValue = parseOr();
                        kwargs->set(Value(name), argValue);
                        hasKwargs = true;
                    } else {
                        args.push_back(parseOr());
                    }
                    if (accept(TokenType::Comma)) continue;
                    consume(TokenType::RParen);
                    break;
                }
                if (hasKwargs) {
                    Value kwv = Value::makeDict(kwargs);
                    kwv.kwargs = true;
                    args.push_back(std::move(kwv));
                }
                value = interp_.callValue(value, args, scope_);
                continue;
            }
            if (accept(TokenType::LBracket)) {
                bool hasSlice = false;
                Value start = Value();
                Value end = Value();
                Value step = Value();
                if (!accept(TokenType::RBracket)) {
                    if (current().type != TokenType::Colon) {
                        start = parseOr();
                    }
                    if (accept(TokenType::Colon)) {
                        hasSlice = true;
                        if (current().type != TokenType::Colon && current().type != TokenType::RBracket) {
                            end = parseOr();
                        }
                        if (accept(TokenType::Colon)) {
                            step = parseOr();
                        }
                    }
                    consume(TokenType::RBracket);
                }
                if (hasSlice) {
                    value = interp_.sliceValue(value, start, end, step);
                } else {
                    value = interp_.indexValue(value, start);
                }
                continue;
            }
            break;
        }
        return value;
    }

    Value parsePrimary() {
        if (accept(TokenType::Number)) {
            const std::string token = tokens_[pos_ - 1].text;
            if (token.find('.') != std::string::npos) {
                return Value(std::stod(token));
            }
            return Value(std::stoll(token));
        }
        if (accept(TokenType::String)) {
            return Value(tokens_[pos_ - 1].text);
        }
        if (accept(TokenType::Identifier)) {
            const std::string name = tokens_[pos_ - 1].text;
            if (name == "true" || name == "True") return Value(true);
            if (name == "false" || name == "False") return Value(false);
            if (name == "null" || name == "None") return Value();
            if (name == "None") return Value();
            return interp_.getVar(name, scope_);
        }
        if (accept(TokenType::LParen)) {
            Value value = parseOr();
            consume(TokenType::RParen);
            return value;
        }
        if (accept(TokenType::LBracket)) {
            std::vector<Value> items;
            if (!accept(TokenType::RBracket)) {
                while (true) {
                    items.push_back(parseOr());
                    if (accept(TokenType::Comma)) continue;
                    consume(TokenType::RBracket);
                    break;
                }
            }
            return Value::makeList(std::make_shared<ValueList>(std::move(items)));
        }
        if (accept(TokenType::LBrace)) {
            auto dict = std::make_shared<Dict>();
            if (!accept(TokenType::RBrace)) {
                while (true) {
                    Value key = parseOr();
                    consume(TokenType::Colon);
                    Value value = parseOr();
                    dict->set(key, std::move(value));
                    if (accept(TokenType::Comma)) continue;
                    consume(TokenType::RBrace);
                    break;
                }
            }
            return Value::makeDict(dict);
        }
        throw ProstoError{"SyntaxError", "unexpected token: " + current().text};
    }
};

static bool compareValues(const Value& a, const std::string& op, const Value& b) {
    if (op == "<") return a < b;
    if (op == ">") return b < a;
    if (op == "<=") return !(b < a);
    if (op == ">=") return !(a < b);
    if (op == "==") return Value::equals(a, b);
    if (op == "!=") return !Value::equals(a, b);
    return false;
}

static bool valueIn(const Value& item, const Value& container) {
    if (container.isString()) {
        return container.s.find(item.toStr()) != std::string::npos;
    }
    if (container.isList()) {
        for (auto& x : *container.list) {
            if (Value::equals(x, item)) return true;
        }
        return false;
    }
    if (container.isDict()) {
        return container.dict->contains(item);
    }
    return false;
}

static int braceDelta(const std::string& line) {
    int d = 0;
    char q = 0;
    for (size_t i = 0; i < line.size(); i++) {
        char c = line[i];
        if (q) {
            if (c == '\\') { i++; continue; }
            if (c == q) q = 0;
            continue;
        }
        if (c == '"' || c == '\'') { q = c; continue; }
        if (c == '#') break;
        if (c == '{') d++;
        else if (c == '}') d--;
    }
    return d;
}

static std::pair<std::vector<std::string>, size_t> getBraceBlock(const std::vector<std::string>& lines, size_t start) {
    size_t i = start;
    while (i < lines.size() && lines[i].find('{') == std::string::npos) i++;
    if (i >= lines.size()) return {{}, lines.size()};

    std::string collected;
    int count = 0;
    size_t j = i;
    for (; j < lines.size(); j++) {
        collected += lines[j];
        collected += "\n";
        count += braceDelta(lines[j]);
        if (count == 0) break;
    }

    size_t first = collected.find('{');
    size_t last = collected.rfind('}');
    std::string inner;
    if (first != std::string::npos && last != std::string::npos && last > first) {
        inner = collected.substr(first + 1, last - first - 1);
    }

    std::vector<std::string> out;
    if (!trim(inner).empty()) out = splitLines(inner);
    return {out, j + 1};
}

static std::vector<std::string> parseParams(const std::string& s) {
    std::vector<std::string> out;
    for (auto& p : splitString(s, ",")) {
        std::string x = trim(p);
        if (!x.empty()) out.push_back(x);
    }
    return out;
}

static bool matchKeywordExpr(const std::string& line, const std::string& kw, std::string& expr) {
    try {
        std::regex re1("^" + kw + "\\s*\\((.+)\\)\\s*\\{?$");
        std::regex re2("^" + kw + "\\s+(.+?)\\s*\\{$");
        std::smatch m;
        if (std::regex_search(line, m, re1)) {
            expr = m[1].str();
            return true;
        }
        if (std::regex_search(line, m, re2)) {
            expr = m[1].str();
            return true;
        }
    } catch (...) {}
    return false;
}

static std::vector<std::string> SKIP_KEYWORDS = {
    "if", "elif", "else", "while", "def", "try", "except",
    "circulate", "multithreading", "multiprocess", "print",
    "return", "break", "continue", "global", "switch", "class",
    "case", "default", "import_pkg", "import"
};

static bool startsWithSkipKeyword(const std::string& s) {
    for (auto& k : SKIP_KEYWORDS) {
        if (s == k || startsWith(s, k + " ") || startsWith(s, k + "{") || startsWith(s, k + "(")) {
            return true;
        }
    }
    return false;
}

static std::optional<std::vector<Value>> parseSequence(Interpreter& interp, const std::string& expr, std::shared_ptr<Scope> scope) {
    std::regex rangeRe(R"(^(-?\d+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)$)");
    std::smatch m;
    std::string e = trim(expr);
    if (std::regex_match(e, m, rangeRe)) {
        long long a = std::stoll(m[1].str());
        long long b = std::stoll(m[2].str());
        long long c = std::stoll(m[3].str());
        std::vector<Value> out;
        if (c > 0) for (long long x = a; x <= b; x += c) out.push_back(Value(x));
        if (c < 0) for (long long x = a; x >= b; x += c) out.push_back(Value(x));
        return out;
    }

    Value v = interp.evalExpr(e, scope);
    if (v.isList()) {
        return *v.list;
    }
    return std::nullopt;
}

static std::vector<std::string> parseExceptTypes(const std::string& line) {
    std::vector<std::string> out;
    std::regex re(R"(^except\s*(?:\(([^)]*)\))?)");
    std::smatch m;
    if (std::regex_search(line, m, re) && m.size() > 1 && m[1].matched) {
        for (auto& t : splitString(m[1].str(), ",")) {
            std::string x = trim(t);
            if (!x.empty()) out.push_back(x);
        }
    }
    return out;
}

static bool exceptionMatches(const std::string& type, const std::vector<std::string>& allowed) {
    if (allowed.empty()) return true;
    for (auto& a : allowed) {
        if (a == type || a == "Exception") return true;
    }
    return false;
}

} // anonymous namespace

Interpreter::Interpreter() {
    registerBuiltins();
}

Interpreter::~Interpreter() = default;

std::unordered_map<std::string, Value> Interpreter::globals() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return globals_;
}

std::unordered_map<std::string, std::shared_ptr<Function>> Interpreter::functions() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return functions_;
}

Value Interpreter::getVar(const std::string& name, std::shared_ptr<Scope> scope) {
    if (scope) {
        for (auto sp = scope; sp; sp = sp->parent) {
            auto it = sp->vars.find(name);
            if (it != sp->vars.end()) return it->second;
        }
    }

    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto it = globals_.find(name);
    if (it != globals_.end()) return it->second;

    auto cit = classes_.find(name);
    if (cit != classes_.end()) return Value::makeClass(cit->second);

    auto fit = functions_.find(name);
    if (fit != functions_.end()) return Value::makeFunction(fit->second);

    auto bit = builtins_.find(name);
    if (bit != builtins_.end()) return bit->second;

    throw ProstoError{"NameError", "name '" + name + "' is not defined"};
}

void Interpreter::assignVar(const std::string& name, Value val, std::shared_ptr<Scope> scope) {
    if (!scope) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        globals_[name] = std::move(val);
        return;
    }
    for (auto sp = scope; sp; sp = sp->parent) {
        if (sp->globalNames.count(name)) {
            std::lock_guard<std::recursive_mutex> lock(mutex_);
            globals_[name] = std::move(val);
            return;
        }
    }
    scope->vars[name] = std::move(val);
}

Value Interpreter::callValue(Value callee, std::vector<Value>& args, std::shared_ptr<Scope> scope) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (callee.isFunction()) {
        auto f = callee.func;
        if (f->native) {
            if (!f->nativeFn) return Value();
            return f->nativeFn(*this, args);
        }
        auto child = std::make_shared<Scope>();
        child->parent = scope;
        for (size_t i = 0; i < f->params.size(); i++) {
            child->vars[f->params[i]] = (i < args.size()) ? args[i] : Value();
        }
        callStack_.push_back({f->name, 0});
        try {
            runLines(f->block, child, 1);
        } catch (ReturnSignal& r) {
            callStack_.pop_back();
            return r.value;
        }
        callStack_.pop_back();
        return Value();
    }

    if (callee.isClass()) {
        auto cls = callee.cls;
        auto obj = std::make_shared<Object>();
        obj->cls = cls;
        auto init = cls->methods.find("__init__");
        if (init != cls->methods.end()) {
            auto child = std::make_shared<Scope>();
            child->parent = scope;
            child->vars["self"] = Value::makeObject(obj);
            for (size_t i = 0; i < init->second->params.size(); i++) {
                if (i == 0) continue;
                child->vars[init->second->params[i]] = (i - 1 < args.size()) ? args[i - 1] : Value();
            }
            callStack_.push_back({"<constructor>", 0});
            try {
                runLines(init->second->block, child, 1);
            } catch (ReturnSignal&) {
            }
            callStack_.pop_back();
        }
        return Value::makeObject(obj);
    }
    throw ProstoError{"TypeError", "object is not callable"};
}

void Interpreter::importPackage(const std::string& path, std::shared_ptr<Scope> scope, int level) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (importedPackages_.count(path)) return;
    importedPackages_.insert(path);

    std::string base = "prosto/package";
    std::string pkgDir;

    if (fs::is_directory(fs::path(base) / path)) {
        pkgDir = (fs::path(base) / path).string();
    } else if (fs::is_directory(base)) {
        for (auto& e : fs::directory_iterator(base)) {
            auto about = e.path() / "about.json";
            if (fs::is_regular_file(about)) {
                try {
                    json j = json::parse(readFileAll(about.string()));
                    if (j.value("name", "") == path) {
                        pkgDir = e.path().string();
                        break;
                    }
                } catch (...) {
                }
            }
        }
    }

    if (pkgDir.empty()) {
        std::cout << "Error [line " << level << "]: Package '" << path << "' not found in " << base << "/" << std::endl;
        return;
    }

    fs::path resDir = fs::path(pkgDir) / "res";
    if (fs::is_directory(resDir)) {
        std::vector<fs::path> files;
        for (auto& e : fs::directory_iterator(resDir)) files.push_back(e.path());
        std::sort(files.begin(), files.end());
        for (auto& f : files) {
            bool exe = f.extension() == ".exe" || f.extension() == ".bat" || f.extension() == ".sh";
            if (exe) {
                std::vector<std::string> argv;
#ifdef _WIN32
                argv.push_back(f.string());
#else
                argv.push_back(f.string());
#endif
                spawnProcess(argv, resDir.string(), "", "");
            }
        }
    }

    std::string entry = (fs::path(pkgDir) / "main_init.ptcp").string();
    if (!fs::exists(entry)) entry = (fs::path(pkgDir) / "main_init.ptc").string();
    if (fs::exists(entry)) {
        auto lines = splitLines(readFileAll(entry));
        runLines(lines, scope, 1);
    } else {
        std::cout << "Error [line " << level << "]: Entry 'main_init.ptcp' not found in '" << path << "'" << std::endl;
    }
}

void Interpreter::printError(const ProstoError& e, int ln) {
    std::cout << "Error [line " << ln << "]: " << e.msg << std::endl;
    for (auto it = callStack_.rbegin(); it != callStack_.rend(); ++it) {
        std::cout << "    at " << it->first << "() [line " << it->second << "]" << std::endl;
    }
}

Value Interpreter::getAttr(const Value& obj, const std::string& name) {
    if (isDunder(name)) throw SecurityError{"Blocked: '." + name + "'"};

    if (obj.isString()) {
        std::string s = obj.s;
        if (name == "upper") return Value::makeNativeFunction([s](Interpreter&, std::vector<Value>&) { return Value(upperString(s)); }, "upper");
        if (name == "lower") return Value::makeNativeFunction([s](Interpreter&, std::vector<Value>&) { return Value(lowerString(s)); }, "lower");
        if (name == "strip") return Value::makeNativeFunction([s](Interpreter&, std::vector<Value>&) { return Value(trim(s)); }, "strip");
        if (name == "split") return Value::makeNativeFunction([s](Interpreter&, std::vector<Value>& args) {
            std::string sep;
            if (!args.empty() && !args[0].isNull()) sep = args[0].toStr();
            auto parts = splitString(s, sep);
            std::vector<Value> out;
            for (auto& p : parts) out.push_back(Value(p));
            return makeListFromVector(out);
        }, "split");
        if (name == "replace") return Value::makeNativeFunction([s](Interpreter&, std::vector<Value>& args) {
            if (args.size() < 2) throw ProstoError{"TypeError", "replace(old,new) requires 2 arguments"};
            std::string old = args[0].toStr();
            std::string neu = args[1].toStr();
            std::string out = s;
            size_t pos = 0;
            while ((pos = out.find(old, pos)) != std::string::npos) {
                out.replace(pos, old.size(), neu);
                pos += neu.size();
            }
            return Value(out);
        }, "replace");
        if (name == "find") return Value::makeNativeFunction([s](Interpreter&, std::vector<Value>& args) {
            if (args.empty()) throw ProstoError{"TypeError", "find(sub) requires 1 argument"};
            auto pos = s.find(args[0].toStr());
            return Value(pos == std::string::npos ? -1LL : (long long)pos);
        }, "find");
        if (name == "startswith") return Value::makeNativeFunction([s](Interpreter&, std::vector<Value>& args) {
            if (args.empty()) return Value(false);
            return Value(startsWith(s, args[0].toStr()));
        }, "startswith");
        if (name == "endswith") return Value::makeNativeFunction([s](Interpreter&, std::vector<Value>& args) {
            if (args.empty()) return Value(false);
            return Value(endsWith(s, args[0].toStr()));
        }, "endswith");
        if (name == "join") return Value::makeNativeFunction([s](Interpreter&, std::vector<Value>& args) {
            if (args.empty() || !args[0].isList()) throw ProstoError{"TypeError", "join(list) requires list"};
            std::string out;
            for (size_t i = 0; i < args[0].list->size(); i++) {
                if (i) out += s;
                out += (*args[0].list)[i].toStr();
            }
            return Value(out);
        }, "join");
        if (name == "format") return Value::makeNativeFunction([s](Interpreter&, std::vector<Value>& args) {
            return Value(formatString(s, args));
        }, "format");
    }

    if (obj.isList()) {
        auto l = obj.list;
        if (name == "append") return Value::makeNativeFunction([l](Interpreter&, std::vector<Value>& args) {
            if (!args.empty()) l->push_back(args[0]);
            return Value();
        }, "append");
        if (name == "pop") return Value::makeNativeFunction([l](Interpreter&, std::vector<Value>& args) {
            if (l->empty()) throw ProstoError{"IndexError", "pop from empty list"};
            long long idx = args.empty() ? -1 : args[0].toInt();
            if (idx < 0) idx += (long long)l->size();
            if (idx < 0 || idx >= (long long)l->size()) throw ProstoError{"IndexError", "pop index out of range"};
            Value v = (*l)[idx];
            l->erase(l->begin() + static_cast<std::ptrdiff_t>(idx));
            return v;
        }, "pop");
        if (name == "insert") return Value::makeNativeFunction([l](Interpreter&, std::vector<Value>& args) {
            if (args.size() < 2) throw ProstoError{"TypeError", "insert(index,value) requires 2 arguments"};
            long long idx = args[0].toInt();
            if (idx < 0) idx += (long long)l->size();
            if (idx < 0) idx = 0;
            if (idx > (long long)l->size()) idx = (long long)l->size();
            l->insert(l->begin() + static_cast<std::ptrdiff_t>(idx), args[1]);
            return Value();
        }, "insert");
        if (name == "remove") return Value::makeNativeFunction([l](Interpreter&, std::vector<Value>& args) {
            if (args.empty()) throw ProstoError{"TypeError", "remove(value) requires 1 argument"};
            for (size_t i = 0; i < l->size(); i++) {
                if (Value::equals((*l)[i], args[0])) {
                    l->erase(l->begin() + static_cast<std::ptrdiff_t>(i));
                    return Value();
                }
            }
            throw ProstoError{"ValueError", "list.remove(x): x not in list"};
        }, "remove");
        if (name == "clear") return Value::makeNativeFunction([l](Interpreter&, std::vector<Value>&) {
            l->clear();
            return Value();
        }, "clear");
        if (name == "index") return Value::makeNativeFunction([l](Interpreter&, std::vector<Value>& args) {
            if (args.empty()) throw ProstoError{"TypeError", "index(value) requires 1 argument"};
            for (size_t i = 0; i < l->size(); i++) {
                if (Value::equals((*l)[i], args[0])) return Value((long long)i);
            }
            throw ProstoError{"ValueError", "value not in list"};
        }, "index");
        if (name == "count") return Value::makeNativeFunction([l](Interpreter&, std::vector<Value>& args) {
            if (args.empty()) return Value(0LL);
            long long c = 0;
            for (auto& x : *l) if (Value::equals(x, args[0])) c++;
            return Value(c);
        }, "count");
        if (name == "reverse") return Value::makeNativeFunction([l](Interpreter&, std::vector<Value>&) {
            std::reverse(l->begin(), l->end());
            return Value();
        }, "reverse");
        if (name == "sort") return Value::makeNativeFunction([l](Interpreter&, std::vector<Value>& args) {
            bool rev = !args.empty() && args[0].toBool();
            if (rev) std::sort(l->begin(), l->end(), [](const Value& a, const Value& b) { return lessValue(b, a); });
            else std::sort(l->begin(), l->end(), lessValue);
            return Value();
        }, "sort");
        if (name == "extend") return Value::makeNativeFunction([l](Interpreter&, std::vector<Value>& args) {
            if (!args.empty() && args[0].isList()) {
                l->insert(l->end(), args[0].list->begin(), args[0].list->end());
            }
            return Value();
        }, "extend");
    }

    if (obj.isDict()) {
        auto d = obj.dict;
        if (name == "keys") return Value::makeNativeFunction([d](Interpreter&, std::vector<Value>&) {
            std::vector<Value> out;
            for (auto& kv : d->items) out.push_back(kv.first);
            return makeListFromVector(out);
        }, "keys");
        if (name == "values") return Value::makeNativeFunction([d](Interpreter&, std::vector<Value>&) {
            std::vector<Value> out;
            for (auto& kv : d->items) out.push_back(kv.second);
            return makeListFromVector(out);
        }, "values");
        if (name == "items") return Value::makeNativeFunction([d](Interpreter&, std::vector<Value>&) {
            std::vector<Value> out;
            for (auto& kv : d->items) out.push_back(makeListFromVector({kv.first, kv.second}));
            return makeListFromVector(out);
        }, "items");
        if (name == "get") return Value::makeNativeFunction([d](Interpreter&, std::vector<Value>& args) {
            if (args.empty()) return Value();
            Value def = args.size() > 1 ? args[1] : Value();
            return d->get(args[0], def);
        }, "get");
        if (name == "pop") return Value::makeNativeFunction([d](Interpreter&, std::vector<Value>& args) {
            if (args.empty()) throw ProstoError{"TypeError", "pop(key) requires 1 argument"};
            auto p = d->find(args[0]);
            if (!p) throw ProstoError{"KeyError", args[0].repr()};
            Value v = *p;
            d->erase(args[0]);
            return v;
        }, "pop");
        if (name == "update") return Value::makeNativeFunction([d](Interpreter&, std::vector<Value>& args) {
            if (!args.empty() && args[0].isDict()) {
                for (auto& kv : args[0].dict->items) d->set(kv.first, kv.second);
            }
            return Value();
        }, "update");
        if (name == "clear") return Value::makeNativeFunction([d](Interpreter&, std::vector<Value>&) {
            d->items.clear();
            return Value();
        }, "clear");
    }

    if (obj.isObject()) {
        auto o = obj.obj;
        auto p = o->attrs->find(Value(name));
        if (p) return *p;
        if (o->cls) {
            auto it = o->cls->methods.find(name);
            if (it != o->cls->methods.end()) {
                auto func = it->second;
                return Value::makeNativeFunction([o, func](Interpreter& in, std::vector<Value>& args) {
                    std::vector<Value> all;
                    all.push_back(Value::makeObject(o));
                    all.insert(all.end(), args.begin(), args.end());
                    return in.callValue(Value::makeFunction(func), all, nullptr);
                }, name);
            }
        }
        throw ProstoError{"AttributeError", "object has no attribute '" + name + "'"};
    }

    if (obj.isNativeObject()) {
        return obj.nat->getAttr(*this, name);
    }

    if (obj.isDict()) {
        auto p = obj.dict->find(Value(name));
        if (p) return *p;
    }

    throw ProstoError{"AttributeError", "value has no attribute '" + name + "'"};
}

void Interpreter::setAttr(Value& obj, const std::string& name, const Value& val) {
    if (obj.isObject()) {
        obj.obj->attrs->set(Value(name), val);
        return;
    }
    if (obj.isDict()) {
        obj.dict->set(Value(name), val);
        return;
    }
    throw ProstoError{"AttributeError", "cannot set attribute '" + name + "' on this object"};
}

Value Interpreter::indexValue(const Value& obj, const Value& idx) {
    if (obj.isList()) {
        long long i = idx.toInt();
        long long n = (long long)obj.list->size();
        if (i < 0) i += n;
        if (i < 0 || i >= n) throw ProstoError{"IndexError", "list index out of range"};
        return (*obj.list)[static_cast<size_t>(i)];
    }
    if (obj.isString()) {
        long long i = idx.toInt();
        long long n = (long long)obj.s.size();
        if (i < 0) i += n;
        if (i < 0 || i >= n) throw ProstoError{"IndexError", "string index out of range"};
        return Value(std::string(1, obj.s[static_cast<size_t>(i)]));
    }
    if (obj.isDict()) {
        auto p = obj.dict->find(idx);
        if (!p) throw ProstoError{"KeyError", idx.repr()};
        return *p;
    }
    throw ProstoError{"TypeError", "value is not indexable"};
}

Value Interpreter::sliceValue(const Value& obj, const Value& start, const Value& end, const Value& step) {
    long long st = step.isNull() ? 1 : step.toInt();
    if (st == 0) throw ProstoError{"ValueError", "slice step cannot be zero"};
    auto normalize = [&](long long x, long long n, long long def) {
        if (x < 0) x += n;
        if (x < 0) x = (st > 0 ? 0 : -1);
        if (x > n) x = (st > 0 ? n : n - 1);
        return x;
    };
    if (obj.isList()) {
        long long n = (long long)obj.list->size();
        long long a = start.isNull() ? (st > 0 ? 0 : n - 1) : normalize(start.toInt(), n, 0);
        long long b = end.isNull() ? (st > 0 ? n : -1) : normalize(end.toInt(), n, n);
        std::vector<Value> out;
        if (st > 0) {
            for (long long i = a; i < b; i += st) out.push_back((*obj.list)[static_cast<size_t>(i)]);
        } else {
            for (long long i = a; i > b; i += st) out.push_back((*obj.list)[static_cast<size_t>(i)]);
        }
        return makeListFromVector(out);
    }
    if (obj.isString()) {
        long long n = (long long)obj.s.size();
        long long a = start.isNull() ? (st > 0 ? 0 : n - 1) : normalize(start.toInt(), n, 0);
        long long b = end.isNull() ? (st > 0 ? n : -1) : normalize(end.toInt(), n, n);
        std::string out;
        if (st > 0) {
            for (long long i = a; i < b; i += st) out.push_back(obj.s[static_cast<size_t>(i)]);
        } else {
            for (long long i = a; i > b; i += st) out.push_back(obj.s[static_cast<size_t>(i)]);
        }
        return Value(out);
    }
    throw ProstoError{"TypeError", "value is not sliceable"};
}

Value Interpreter::evalExpr(const std::string& expr, std::shared_ptr<Scope> scope) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    ExprParserImpl parser(*this, trim(expr), scope);
    return parser.parse();
}

void Interpreter::runLines(const std::vector<std::string>& lines, std::shared_ptr<Scope> scope, int baseLine) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    size_t i = 0;
    bool inMultilineComment = false;

    while (i < lines.size()) {
        std::string line = lines[i];
        if (!line.empty() && line.back() == '\n') line.pop_back();
        std::string stripped = trim(line);
        int ln = baseLine + static_cast<int>(i);

        if (inMultilineComment) {
            if (endsWith(stripped, "--")) inMultilineComment = false;
            i++;
            continue;
        }

        if (startsWith(stripped, "--")) {
            if (!endsWith(stripped, "--")) inMultilineComment = true;
            i++;
            continue;
        }

        if (stripped.empty() || startsWith(stripped, "#")) {
            i++;
            continue;
        }

        if (stripped == "return" || startsWith(stripped, "return ")) {
            std::string expr = trim(stripped.substr(6));
            if (expr.empty()) throw ReturnSignal{Value()};
            throw ReturnSignal{evalExpr(expr, scope)};
        }

        if (stripped == "break") throw BreakSignal{};
        if (stripped == "continue") throw ContinueSignal{};

        std::smatch m;

        if (std::regex_search(stripped, m, std::regex(R"(^global\s+([\w, ]+))"))) {
            for (auto& n : splitString(m[1].str(), ",")) {
                std::string x = trim(n);
                if (!x.empty() && scope) scope->globalNames.insert(x);
            }
            i++;
            continue;
        }

        if (std::regex_search(stripped, m, std::regex(R"(^import\s*<\s*([^>]+)\s*>)"))) {
            std::string fname = trim(m[1].str());
            if (!importedFiles_.count(fname)) {
                importedFiles_.insert(fname);
                std::string target = fname;
                if (!fs::exists(target) && endsWith(target, ".ptc")) target += "p";
                if (!fs::exists(target) && !endsWith(target, ".ptcp")) target += ".ptcp";
                if (fs::exists(target)) {
                    auto content = splitLines(readFileAll(target));
                    runLines(content, scope, 1);
                } else {
                    std::cout << "Error [line " << ln << "]: File '" << fname << "' not found." << std::endl;
                }
            }
            i++;
            continue;
        }

        if (std::regex_search(stripped, m, std::regex(R"(^import_pkg\s+(\w[\w-]*))"))) {
            importPackage(m[1].str(), scope, ln);
            i++;
            continue;
        }

        if (!startsWithSkipKeyword(stripped)) {
            if (std::regex_search(stripped, m, std::regex(R"(^([A-Za-z_]\w*(?:\.[A-Za-z_]\w*)*)\.([A-Za-z_]\w*)\s*=(?!=)\s*(.+)$)"))) {
                std::string objExpr = m[1].str();
                std::string attr = m[2].str();
                std::string rhs = m[3].str();
                std::vector<std::string> parts = splitString(objExpr, ".");
                Value obj = getVar(parts[0], scope);
                for (size_t k = 1; k < parts.size(); k++) {
                    obj = getAttr(obj, parts[k]);
                }
                setAttr(obj, attr, evalExpr(rhs, scope));
                i++;
                continue;
            }

            if (std::regex_search(stripped, m, std::regex(R"(^([A-Za-z_]\w*)\s*(\+=|-=|\*=|/=|%=|\*\*=|//=)\s*(.+)$)"))) {
                std::string var = m[1].str();
                std::string op = m[2].str();
                std::string rhs = m[3].str();
                Value old = getVar(var, scope);
                Value rv = evalExpr(rhs, scope);
                std::string binop = op.substr(0, op.size() - 1);
                assignVar(var, prosto::applyBinary(binop, old, rv), scope);
                i++;
                continue;
            }

            if (std::regex_search(stripped, m, std::regex(R"(^([A-Za-z_][\w, ]*)\s*=(?!=)\s*(.+)$)"))) {
                std::vector<std::string> names;
                for (auto& n : splitString(m[1].str(), ",")) {
                    std::string x = trim(n);
                    if (!x.empty()) names.push_back(x);
                }
                Value val = evalExpr(m[2].str(), scope);
                if (names.size() == 1) {
                    assignVar(names[0], val, scope);
                } else {
                    if (val.isList()) {
                        for (size_t k = 0; k < names.size(); k++) {
                            assignVar(names[k], k < val.list->size() ? (*val.list)[k] : Value(), scope);
                        }
                    } else {
                        for (size_t k = 0; k < names.size(); k++) {
                            assignVar(names[k], k == 0 ? val : Value(), scope);
                        }
                    }
                }
                i++;
                continue;
            }
        }

        if (std::regex_search(stripped, m, std::regex(R"(^class\s+(\w+)\s*\{?)"))) {
            std::string cname = m[1].str();
            auto [block, jump] = getBraceBlock(lines, i);
            auto cls = std::make_shared<Class>();
            cls->name = cname;
            size_t j = 0;
            while (j < block.size()) {
                std::string bl = trim(block[j]);
                std::smatch mm;
                if (std::regex_search(bl, mm, std::regex(R"(^def\s+(\w+)\s*\((.*?)\)\s*\{?)"))) {
                    std::string mn = mm[1].str();
                    std::vector<std::string> mp = parseParams(mm[2].str());
                    auto [mb, mj] = getBraceBlock(block, j);
                    if (mn == "init") mn = "__init__";
                    auto fn = std::make_shared<Function>();
                    fn->name = cname + "." + mn;
                    fn->params = mp;
                    fn->block = mb;
                    fn->method = true;
                    cls->methods[mn] = fn;
                    j = mj;
                } else {
                    j++;
                }
            }
            classes_[cname] = cls;
            {
                std::lock_guard<std::recursive_mutex> lock(mutex_);
                globals_[cname] = Value::makeClass(cls);
            }
            i = jump;
            continue;
        }

        if (std::regex_search(stripped, m, std::regex(R"(^switch\s*\((.+)\)\s*\{?)"))) {
            std::string expr = m[1].str();
            auto [block, jump] = getBraceBlock(lines, i);
            Value sv = evalExpr(expr, scope);
            std::vector<std::pair<std::string, std::vector<std::string>>> cases;
            std::string currentVal;
            std::vector<std::string> currentBlock;
            bool inCase = false;
            for (auto& raw : block) {
                std::string s = trim(raw);
                std::smatch cm;
                if (std::regex_search(s, cm, std::regex(R"(^case\s+(.+?)\s*:)"))) {
                    if (inCase) cases.push_back({currentVal, currentBlock});
                    currentVal = cm[1].str();
                    currentBlock.clear();
                    inCase = true;
                } else if (std::regex_search(s, std::regex(R"(^default\s*:)"))) {
                    if (inCase) cases.push_back({currentVal, currentBlock});
                    currentVal = "__default__";
                    currentBlock.clear();
                    inCase = true;
                } else {
                    if (inCase) currentBlock.push_back(raw);
                }
            }
            if (inCase) cases.push_back({currentVal, currentBlock});
            bool matched = false;
            for (auto& c : cases) {
                if (c.first == "__default__") continue;
                Value cv;
                try { cv = evalExpr(c.first, scope); } catch (...) { cv = Value(c.first); }
                if (Value::equals(cv, sv)) {
                    runLines(c.second, scope, ln + 1);
                    matched = true;
                    break;
                }
            }
            if (!matched) {
                for (auto& c : cases) {
                    if (c.first == "__default__") {
                        runLines(c.second, scope, ln + 1);
                        break;
                    }
                }
            }
            i = jump;
            continue;
        }

        if (stripped == "try" || startsWith(stripped, "try ") || startsWith(stripped, "try{")) {
            auto [block, jump] = getBraceBlock(lines, i);
            try {
                runLines(block, scope, ln + 1);
                i = jump;
                if (i < lines.size() && startsWith(trim(lines[i]), "except")) {
                    auto [b2, j2] = getBraceBlock(lines, i);
                    i = j2;
                }
            } catch (BreakSignal&) {
                throw;
            } catch (ContinueSignal&) {
                throw;
            } catch (ReturnSignal&) {
                throw;
            } catch (ProstoError& e) {
                if (i < lines.size() && startsWith(trim(lines[i]), "except")) {
                    auto types = parseExceptTypes(trim(lines[i]));
                    if (exceptionMatches(e.type, types)) {
                        auto [b2, j2] = getBraceBlock(lines, i);
                        runLines(b2, scope, ln + 1);
                        i = j2;
                    } else {
                        throw;
                    }
                } else {
                    i = jump;
                    throw;
                }
            }
            continue;
        }

        std::string condExpr;
        if (matchKeywordExpr(stripped, "if", condExpr)) {
            auto [block, jump] = getBraceBlock(lines, i);
            if (evalExpr(condExpr, scope).toBool()) {
                runLines(block, scope, ln + 1);
                i = jump;
                while (i < lines.size() && (startsWith(trim(lines[i]), "elif") || startsWith(trim(lines[i]), "else"))) {
                    auto [b2, j2] = getBraceBlock(lines, i);
                    i = j2;
                }
            } else {
                i = jump;
                bool handled = false;
                while (i < lines.size() && startsWith(trim(lines[i]), "elif")) {
                    std::string eexpr;
                    if (matchKeywordExpr(trim(lines[i]), "elif", eexpr)) {
                        auto [b2, j2] = getBraceBlock(lines, i);
                        if (evalExpr(eexpr, scope).toBool()) {
                            runLines(b2, scope, ln + 1);
                            i = j2;
                            handled = true;
                            while (i < lines.size() && (startsWith(trim(lines[i]), "elif") || startsWith(trim(lines[i]), "else"))) {
                                auto [b3, j3] = getBraceBlock(lines, i);
                                i = j3;
                            }
                            break;
                        } else {
                            i = j2;
                        }
                    } else {
                        break;
                    }
                }
                if (!handled && i < lines.size() && startsWith(trim(lines[i]), "else")) {
                    auto [b3, j3] = getBraceBlock(lines, i);
                    runLines(b3, scope, ln + 1);
                    i = j3;
                }
            }
            continue;
        }

        if (matchKeywordExpr(stripped, "while", condExpr)) {
            auto [block, jump] = getBraceBlock(lines, i);
            while (evalExpr(condExpr, scope).toBool()) {
                try {
                    runLines(block, scope, ln + 1);
                } catch (BreakSignal&) {
                    break;
                } catch (ContinueSignal&) {
                    continue;
                }
            }
            i = jump;
            continue;
        }

        if (std::regex_search(stripped, m, std::regex(R"(^def\s+(\w+)\s*\((.*?)\)\s*\{?)"))) {
            std::string fname = m[1].str();
            std::vector<std::string> params = parseParams(m[2].str());
            auto [block, jump] = getBraceBlock(lines, i);
            auto fn = std::make_shared<Function>();
            fn->name = fname;
            fn->params = params;
            fn->block = block;
            functions_[fname] = fn;
            i = jump;
            continue;
        }

        if (std::regex_search(stripped, m, std::regex(R"(^circulate\s*:\s*(\w+)\s*\((.+)\)\s*\{?)"))) {
            std::string var = m[1].str();
            std::string expr = m[2].str();
            auto [block, jump] = getBraceBlock(lines, i);
            auto seq = parseSequence(*this, expr, scope);
            if (seq) {
                for (auto& item : *seq) {
                    auto child = std::make_shared<Scope>();
                    child->parent = scope;
                    child->vars[var] = item;
                    try {
                        runLines(block, child, ln + 1);
                    } catch (BreakSignal&) {
                        break;
                    } catch (ContinueSignal&) {
                        continue;
                    }
                }
            }
            i = jump;
            continue;
        }

        if (std::regex_search(stripped, m, std::regex(R"(^multithreading\s*:\s*(\w+)\s*\((.+)\)\s*\{?)"))) {
            std::string var = m[1].str();
            std::string expr = m[2].str();
            auto [block, jump] = getBraceBlock(lines, i);
            auto seq = parseSequence(*this, expr, scope);
            if (seq) {
                std::vector<std::thread> threads;
                for (auto& item : *seq) {
                    auto child = std::make_shared<Scope>();
                    child->parent = scope;
                    child->vars[var] = item;
                    threads.emplace_back([this, block, child, ln]() {
                        try {
                            runLines(block, child, ln + 1);
                        } catch (ProstoError& e) {
                            std::cout << "Thread Error: " << e.msg << std::endl;
                        } catch (...) {}
                    });
                }
                for (auto& t : threads) t.join();
            }
            i = jump;
            continue;
        }

        if (std::regex_search(stripped, m, std::regex(R"(^multiprocess\s*:\s*(\w+)\s*\((.+)\)\s*\{?)"))) {
            std::string var = m[1].str();
            std::string expr = m[2].str();
            auto [block, jump] = getBraceBlock(lines, i);
            auto seq = parseSequence(*this, expr, scope);
            if (seq) {
                static bool warned = false;
                if (!warned) {
                    std::cout << "Warning [line " << ln << "]: multiprocess uses threads on this platform" << std::endl;
                    warned = true;
                }
                std::vector<std::thread> threads;
                for (auto& item : *seq) {
                    auto child = std::make_shared<Scope>();
                    child->parent = scope;
                    child->vars[var] = item;
                    threads.emplace_back([this, block, child, ln]() {
                        try {
                            runLines(block, child, ln + 1);
                        } catch (ProstoError& e) {
                            std::cout << "Process Error: " << e.msg << std::endl;
                        } catch (...) {}
                    });
                }
                for (auto& t : threads) t.join();
            }
            i = jump;
            continue;
        }

        if (std::regex_search(stripped, m, std::regex(R"(^print\s*\((.*)\)$)"))) {
            std::string argsStr = m[1].str();
            std::string endVal = "\n";
            std::regex endRe(R"((?:^|,)\s*end\s*=\s*("[^"]*"|'[^']*'))");
            std::smatch em;
            std::string contentStr = argsStr;
            if (std::regex_search(argsStr, em, endRe)) {
                Value ev = evalExpr(em[1].str(), scope);
                endVal = ev.toStr();
                contentStr = argsStr.substr(0, em.position()) + argsStr.substr(em.position() + em.length());
                contentStr = trim(contentStr);
                if (!contentStr.empty() && contentStr.front() == ',') contentStr = trim(contentStr.substr(1));
            }
            if (!contentStr.empty()) {
                try {
                    Value val = evalExpr(contentStr, scope);
                    if (val.isList()) {
                        for (auto& x : *val.list) {
                            std::cout << x.toStr() << endVal;
                        }
                    } else {
                        std::cout << val.toStr() << endVal;
                    }
                } catch (...) {
                    std::cout << contentStr << endVal;
                }
            } else {
                std::cout << endVal;
            }
            i++;
            continue;
        }

        try {
            evalExpr(stripped, scope);
        } catch (...) {
        }
        i++;
    }
}

void Interpreter::runFile(const std::string& path) {
    auto lines = splitLines(readFileAll(path));
    runLines(lines, nullptr, 1);
}

} // namespace prosto
