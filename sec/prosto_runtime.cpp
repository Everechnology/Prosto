#include "prosto_common.hpp"

Value makeListFromVector(vector<Value> v) {
    return Value::makeList(make_shared<ValueList>(move(v)));
}

[[maybe_unused]] Value makeDictFromPairs(vector<pair<Value, Value>> pairs) {
    auto d = make_shared<Dict>();
    for (auto& p : pairs) d->set(p.first, p.second);
    return Value::makeDict(d);
}

bool Dict::contains(const Value& k) const {
    return find(k) != nullptr;
}

Value* Dict::find(const Value& k) {
    for (auto& kv : items) {
        if (Value::equals(kv.first, k)) return &kv.second;
    }
    return nullptr;
}

const Value* Dict::find(const Value& k) const {
    for (auto& kv : items) {
        if (Value::equals(kv.first, k)) return &kv.second;
    }
    return nullptr;
}

Value Dict::get(const Value& k, Value def) const {
    auto p = find(k);
    return p ? *p : def;
}

void Dict::set(const Value& k, Value v) {
    if (auto p = find(k)) {
        *p = v;
        return;
    }
    items.push_back({k, v});
}

bool Dict::erase(const Value& k) {
    for (size_t i = 0; i < items.size(); i++) {
        if (Value::equals(items[i].first, k)) {
            items.erase(items.begin() + i);
            return true;
        }
    }
    return false;
}

// Interpreter member storage and method implementations

Interpreter::Interpreter() {
    registerBuiltins();
}

Value Interpreter::getVar(const string& name, shared_ptr<Scope> scope) {
    for (auto sp = scope; sp; sp = sp->parent) {
        auto it = sp->vars.find(name);
        if (it != sp->vars.end()) return it->second;
    }

    {
        lock_guard<recursive_mutex> lock(globalMutex);
        auto it = globals.find(name);
        if (it != globals.end()) return it->second;
    }

    auto cit = classes.find(name);
    if (cit != classes.end()) return Value::makeClass(cit->second);

    auto fit = functions.find(name);
    if (fit != functions.end()) return Value::makeFunction(fit->second);

    auto bit = builtins.find(name);
    if (bit != builtins.end()) return bit->second;

    throw ProstoError{"NameError", "name '" + name + "' is not defined"};
}

void Interpreter::assignVar(const string& name, Value val, shared_ptr<Scope> scope) {
    if (!scope) {
        lock_guard<recursive_mutex> lock(globalMutex);
        globals[name] = val;
        return;
    }

    for (auto sp = scope; sp; sp = sp->parent) {
        if (sp->globalNames.count(name)) {
            lock_guard<recursive_mutex> lock(globalMutex);
            globals[name] = val;
            return;
        }
    }

    scope->vars[name] = val;
}

Value Interpreter::callValue(Value callee, vector<Value>& args, shared_ptr<Scope> scope) {
    if (callee.isFunction()) {
        auto f = callee.func;
        if (f->native) {
            if (!f->nativeFn) return Value();
            return f->nativeFn(*this, args);
        }

        auto child = make_shared<Scope>();
        child->parent = scope;
        for (size_t i = 0; i < f->params.size(); i++) {
            child->vars[f->params[i]] = (i < args.size()) ? args[i] : Value();
        }

        callStack.push_back({f->name, 0});
        try {
            runLines(f->block, child, 1);
        } catch (ReturnSignal& r) {
            callStack.pop_back();
            return r.value;
        }
        callStack.pop_back();
        return Value();
    }

    if (callee.isClass()) {
        auto cls = callee.cls;
        auto obj = make_shared<Object>();
        obj->cls = cls;
        auto init = cls->methods.find("__init__");
        if (init != cls->methods.end()) {
            auto child = make_shared<Scope>();
            child->parent = scope;
            child->vars["self"] = Value::makeObject(obj);
            for (size_t i = 0; i < init->second->params.size(); i++) {
                if (i == 0) continue; // skip 'self'
                child->vars[init->second->params[i]] = (i-1 < args.size()) ? args[i-1] : Value();
            }

            callStack.push_back({"<constructor>", 0});
            try {
                runLines(init->second->block, child, 1);
            } catch (ReturnSignal& r) {
                // ignore return in constructor
            }
            callStack.pop_back();
        }
        return Value::makeObject(obj);
    }

    throw ProstoError{"TypeError", "object is not callable"};
}

void Interpreter::importPackage(const std::string& path, std::shared_ptr<Scope> scope, int level) {
    if (importedPackages.count(path)) return;
    importedPackages.insert(path);

    string base = "prosto/package";
    string pkgDir;

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
                } catch (...) {}
            }
        }
    }

    if (pkgDir.empty()) {
        cout << "Error [line " << level << "]: Package '" << path << "' not found in " << base << "/" << endl;
        return;
    }

    fs::path resDir = fs::path(pkgDir) / "res";
    if (fs::is_directory(resDir)) {
        vector<fs::path> files;
        for (auto& e : fs::directory_iterator(resDir)) files.push_back(e.path());
        sort(files.begin(), files.end());
        for (auto& f : files) {
            bool exe = f.extension() == ".exe" || f.extension() == ".bat" || f.extension() == ".sh";
            if (exe) {
                string cmd = "cd \"" + resDir.string() + "\" && \"" + f.string() + "\"";
                system(cmd.c_str());
            }
        }
    }

    string entry = (fs::path(pkgDir) / "main_init.ptcp").string();
    if (!fs::exists(entry)) entry = (fs::path(pkgDir) / "main_init.ptc").string();

    if (fs::exists(entry)) {
        auto lines = splitLines(readFileAll(entry));
        runLines(lines, scope, 1);
    } else {
        cout << "Error [line " << level << "]: Entry 'main_init.ptcp' not found in '" << path << "'" << endl;
    }
}

void Interpreter::printError(const ProstoError& e, int ln) {
    cout << "Error [line " << ln << "]: " << e.msg << endl;
    for (auto it = callStack.rbegin(); it != callStack.rend(); ++it) {
        cout << "    at " << it->first << "() [line " << it->second << "]" << endl;
    }
}

Value applyBinary(const string& op, const Value& a, const Value& b) {
    if (op == "+") {
        if (a.isNumber() && b.isNumber()) {
            if (a.type == VT::Int && b.type == VT::Int) return Value(a.i + b.i);
            return Value(a.toFloat() + b.toFloat());
        }
        if (a.isString() && b.isString()) return Value(a.s + b.s);
        if (a.isList() && b.isList()) {
            auto out = make_shared<ValueList>();
            for (auto& x : *a.list) out->push_back(x);
            for (auto& x : *b.list) out->push_back(x);
            return Value::makeList(out);
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
            string out;
            for (long long i = 0; i < b.i; i++) out += a.s;
            return Value(out);
        }
        if (a.isList() && b.type == VT::Int) {
            auto out = make_shared<ValueList>();
            for (long long i = 0; i < b.i; i++) {
                for (auto& x : *a.list) out->push_back(x);
            }
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
                long long q = a.i / b.i;
                long long r = a.i % b.i;
                if (r != 0 && ((r < 0) != (b.i < 0))) q--;
                return Value(q);
            }
            double rb = b.toFloat();
            if (rb == 0.0) throw ProstoError{"ZeroDivisionError", "division by zero"};
            return Value(floor(a.toFloat() / rb));
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
            double r = fmod(a.toFloat(), b.toFloat());
            if (r != 0 && ((r < 0) != (b.i < 0))) r += b.i;
            return Value(r);
        }
        throw ProstoError{"TypeError", "unsupported operand types for %"};
    }

    if (op == "**") {
        if (a.type == VT::Int && b.type == VT::Int && b.i >= 0) {
            long long res = 1;
            for (long long i = 0; i < b.i; i++) res *= a.i;
            return Value(res);
        }
        return Value(pow(a.toFloat(), b.toFloat()));
    }

    if (op == "&" || op == "|" || op == "^" || op == "<<" || op == ">>") {
        long long x = a.toInt();
        long long y = b.toInt();
        if (op == "&") return Value(x & y);
        if (op == "|") return Value(x | y);
        if (op == "^") return Value(x ^ y);
        if (op == "<<") return Value(x << y);
        if (op == ">>") return Value(x >> y);
    }

    throw ProstoError{"RuntimeError", "unknown operator: " + op};
}

bool lessValue(const Value& a, const Value& b) { return a < b; }

bool compareValues(const Value& a, const std::string& op, const Value& b) {
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
        return container.s.find(item.toStr()) != string::npos;
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
        default:
            return v.toStr();
    }
}

Value jsonToValue(const json& j) {
    if (j.is_null()) return Value();
    if (j.is_boolean()) return Value(j.get<bool>());
    if (j.is_number_integer()) return Value((long long)j.get<int64_t>());
    if (j.is_number_unsigned()) return Value((long long)j.get<uint64_t>());
    if (j.is_number_float()) return Value(j.get<double>());
    if (j.is_string()) return Value(j.get<string>());
    if (j.is_array()) {
        vector<Value> arr;
        for (auto& x : j) arr.push_back(jsonToValue(x));
        return makeListFromVector(arr);
    }
    if (j.is_object()) {
        auto d = make_shared<Dict>();
        for (auto it = j.begin(); it != j.end(); ++it) {
            d->set(Value(it.key()), jsonToValue(it.value()));
        }
        return Value::makeDict(d);
    }
    return Value();
}

static string hexEncode(const unsigned char* data, size_t len) {
    static const char* tbl = "0123456789abcdef";
    string out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; i++) {
        out += tbl[data[i] >> 4];
        out += tbl[data[i] & 15];
    }
    return out;
}

string hexEncodeString(const string& s) {
    return hexEncode((const unsigned char*)s.data(), s.size());
}

string hexDecodeString(const string& s) {
    string out;
    for (size_t i = 0; i + 1 < s.size(); i += 2) {
        int hi = 0, lo = 0;
        char c1 = s[i], c2 = s[i + 1];
        if (c1 >= '0' && c1 <= '9') hi = c1 - '0';
        else if (c1 >= 'a' && c1 <= 'f') hi = c1 - 'a' + 10;
        else if (c1 >= 'A' && c1 <= 'F') hi = c1 - 'A' + 10;
        if (c2 >= '0' && c2 <= '9') lo = c2 - '0';
        else if (c2 >= 'a' && c2 <= 'f') lo = c2 - 'a' + 10;
        else if (c2 >= 'A' && c2 <= 'F') lo = c2 - 'A' + 10;
        out += (char)((hi << 4) | lo);
    }
    return out;
}

string base64EncodeImpl(const unsigned char* data, size_t len, bool url) {
    string tbl = url
        ? "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"
        : "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    string out;
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

string base64DecodeImpl(const string& in, bool url) {
    vector<int> T(256, -1);
    string tbl = url
        ? "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"
        : "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (int i = 0; i < 64; i++) T[(unsigned char)tbl[i]] = i;
    string out;
    int val = 0, valb = -8;
    for (unsigned char c : in) {
        if (T[c] == -1) break;
        val = (val << 6) + T[c];
        valb += 6;
        if (valb >= 0) {
            out.push_back(char((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return out;
}

string urlEncodeString(const string& s) {
    static const char* hex = "0123456789ABCDEF";
    string out;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += (char)c;
        else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

string urlDecodeString(const string& s) {
    string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size()) {
            int hi = 0, lo = 0;
            char c1 = s[i + 1], c2 = s[i + 2];
            if (c1 >= '0' && c1 <= '9') hi = c1 - '0';
            else if (c1 >= 'a' && c1 <= 'f') hi = c1 - 'a' + 10;
            else if (c1 >= 'A' && c1 <= 'F') hi = c1 - 'A' + 10;
            if (c2 >= '0' && c2 <= '9') lo = c2 - '0';
            else if (c2 >= 'a' && c2 <= 'f') lo = c2 - 'a' + 10;
            else if (c2 >= 'A' && c2 <= 'F') lo = c2 - 'A' + 10;
            out += (char)((hi << 4) | lo);
            i += 2;
        } else if (s[i] == '+') out += ' ';
        else out += s[i];
    }
    return out;
}

string htmlEscapeString(const string& s) {
    string out;
    for (char c : s) {
        if (c == '&') out += "&amp;";
        else if (c == '<') out += "&lt;";
        else if (c == '>') out += "&gt;";
        else if (c == '"') out += "&quot;";
        else if (c == '\'') out += "&#39;";
        else out += c;
    }
    return out;
}

string htmlUnescapeString(const string& s) {
    string out = s;
    static vector<pair<string, string>> reps = {
        {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&#39;", "'"}, {"&apos;", "'"}, {"&amp;", "&"}
    };
    for (auto& r : reps) {
        size_t pos = 0;
        while ((pos = out.find(r.first, pos)) != string::npos) {
            out.replace(pos, r.first.size(), r.second);
            pos += r.second.size();
        }
    }
    return out;
}

uint32_t crc32String(const string& s) {
    uint32_t crc = 0xFFFFFFFF;
    for (unsigned char c : s) {
        crc ^= c;
        for (int k = 0; k < 8; k++) {
            crc = (crc >> 1) ^ (0xEDB88320 & (uint32_t)(-(int32_t)(crc & 1)));
        }
    }
    return crc ^ 0xFFFFFFFF;
}

string evpDigestHex(const string& algo, const unsigned char* data, size_t len) {
    const EVP_MD* md = EVP_get_digestbyname(algo.c_str());
    if (!md) throw ProstoError{"ValueError", "unsupported digest: " + algo};
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    unsigned char out[EVP_MAX_MD_SIZE];
    unsigned int outlen = 0;
    EVP_DigestInit_ex(ctx, md, nullptr);
    EVP_DigestUpdate(ctx, data, len);
    EVP_DigestFinal_ex(ctx, out, &outlen);
    EVP_MD_CTX_free(ctx);
    return hexEncode(out, outlen);
}

string evpFileDigestHex(const string& path, const string& algo) {
    const EVP_MD* md = EVP_get_digestbyname(algo.c_str());
    if (!md) throw ProstoError{"ValueError", "unsupported digest: " + algo};
    ifstream f(path, ios::binary);
    if (!f) throw ProstoError{"FileNotFoundError", "file not found: " + path};
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    unsigned char out[EVP_MAX_MD_SIZE];
    unsigned int outlen = 0;
    EVP_DigestInit_ex(ctx, md, nullptr);
    char buf[8192];
    while (f.read(buf, sizeof(buf)) || f.gcount()) {
        EVP_DigestUpdate(ctx, buf, (size_t)f.gcount());
        if (f.eof()) break;
    }
    EVP_DigestFinal_ex(ctx, out, &outlen);
    EVP_MD_CTX_free(ctx);
    return hexEncode(out, outlen);
}

string hmacSha256Hex(const string& key, const string& msg) {
    unsigned char out[EVP_MAX_MD_SIZE];
    unsigned int outlen = 0;
    HMAC(EVP_sha256(),
         key.data(), (int)key.size(),
         (const unsigned char*)msg.data(), msg.size(),
         out, &outlen);
    return hexEncode(out, outlen);
}

string trim(const string& s) {
    size_t a = 0, b = s.size();
    while (a < b && isspace((unsigned char)s[a])) a++;
    while (b > a && isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}

bool startsWith(const string& s, const string& p) {
    return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}

bool endsWith(const string& s, const string& p) {
    return s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0;
}

string upperString(const string& s) {
    string out = s;
    for (auto& c : out) c = (char)toupper((unsigned char)c);
    return out;
}

string lowerString(const string& s) {
    string out = s;
    for (auto& c : out) c = (char)tolower((unsigned char)c);
    return out;
}

vector<string> splitString(const string& s, const string& sep) {
    vector<string> out;
    if (sep.empty()) {
        istringstream iss(s);
        string tok;
        while (iss >> tok) out.push_back(tok);
        return out;
    }
    size_t start = 0;
    while (true) {
        size_t pos = s.find(sep, start);
        if (pos == string::npos) {
            out.push_back(s.substr(start));
            break;
        }
        out.push_back(s.substr(start, pos - start));
        start = pos + sep.size();
    }
    return out;
}

vector<string> splitLines(const string& s) {
    vector<string> lines;
    string cur;
    for (char c : s) {
        if (c == '\n') {
            lines.push_back(cur);
            cur.clear();
        } else if (c != '\r') cur += c;
    }
    if (!cur.empty() || !s.empty()) lines.push_back(cur);
    return lines;
}

string readFileAll(const string& path) {
    ifstream f(path, ios::binary);
    if (!f) throw ProstoError{"FileNotFoundError", "file not found: " + path};
    ostringstream oss;
    oss << f.rdbuf();
    return oss.str();
}

void writeFileAll(const string& path, const string& text, bool append) {
    fs::path p(path);
    if (p.has_parent_path()) fs::create_directories(p.parent_path());
    ofstream f(path, append ? ios::app : ios::trunc);
    if (!f) throw ProstoError{"OSError", "cannot open file: " + path};
    f << text;
}

string generateUUID() {
    static uniform_int_distribution<int> dist(0, 15);
    static uniform_int_distribution<int> dist2(8, 11);
    const char* hex = "0123456789abcdef";
    string u;
    for (int i = 0; i < 36; i++) {
        if (i == 8 || i == 13 || i == 18 || i == 23) u += '-';
        else if (i == 14) u += '4';
        else if (i == 19) u += hex[dist2(RNG)];
        else u += hex[dist(RNG)];
    }
    return u;
}

string formatDate(const string& fmt) {
    time_t t = time(nullptr);
    tm tmv{};
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    char buf[512];
    strftime(buf, sizeof(buf), fmt.c_str(), &tmv);
    return string(buf);
}

string platformName() {
#ifdef _WIN32
    return "Windows";
#elif __APPLE__
    return "Darwin";
#else
    return "Linux";
#endif
}

string fileMtimeString(const string& path) {
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
    return string(buf);
}

string globToRegex(const string& pat) {
    string out = "^";
    for (char c : pat) {
        if (c == '*') out += ".*";
        else if (c == '?') out += ".";
        else if (strchr(".^$+()[]{}|\\", c)) {
            out += "\\";
            out += c;
        } else out += c;
    }
    out += "$";
    return out;
}

vector<string> globFiles(const string& pattern) {
    fs::path p(pattern);
    fs::path dir = p.parent_path();
    string file = p.filename().string();
    if (dir.empty()) dir = ".";
    vector<string> out;
    if (!fs::exists(dir) || !fs::is_directory(dir)) return out;
    regex re(globToRegex(file));
    for (auto& e : fs::directory_iterator(dir)) {
        string name = e.path().filename().string();
        if (regex_match(name, re)) out.push_back(e.path().generic_string());
    }
    sort(out.begin(), out.end());
    return out;
}

string formatString(const string& fmt, const vector<Value>& args) {
    string out;
    size_t autoIdx = 0;
    for (size_t i = 0; i < fmt.size(); i++) {
        if (fmt[i] == '{') {
            if (i + 1 < fmt.size() && fmt[i + 1] == '{') {
                out += '{';
                i++;
                continue;
            }
            size_t end = fmt.find('}', i);
            if (end == string::npos) {
                out += fmt[i];
                continue;
            }
            string key = trim(fmt.substr(i + 1, end - i - 1));
            Value val;
            if (key.empty()) {
                if (autoIdx < args.size()) val = args[autoIdx++];
            } else if (all_of(key.begin(), key.end(), ::isdigit)) {
                size_t idx = stoul(key);
                if (idx < args.size()) val = args[idx];
            } else if (!args.empty() && args.back().isDict()) {
                val = args.back().dict->get(Value(key), Value("{" + key + "}"));
            } else {
                val = Value("{" + key + "}");
            }
            out += val.toStr();
            i = end;
        } else if (fmt[i] == '}') {
            if (i + 1 < fmt.size() && fmt[i + 1] == '}') {
                out += '}';
                i++;
            } else out += '}';
        } else out += fmt[i];
    }
    return out;
}

string templateString(const string& tmpl, const Value& data) {
    string out;
    for (size_t i = 0; i < tmpl.size(); i++) {
        if (tmpl[i] == '$' && i + 1 < tmpl.size() && tmpl[i + 1] == '{') {
            size_t end = tmpl.find('}', i + 2);
            if (end == string::npos) {
                out += tmpl[i];
                continue;
            }
            string key = tmpl.substr(i + 2, end - i - 2);
            if (data.isDict()) {
                out += data.dict->get(Value(key), Value("${" + key + "}")).toStr();
            } else {
                out += "${" + key + "}";
            }
            i = end;
        } else out += tmpl[i];
    }
    return out;
}

[[maybe_unused]] Value getKwargsDict(vector<Value>& args) {
    if (!args.empty() && args.back().kwargs && args.back().isDict()) {
        Value d = args.back();
        args.pop_back();
        return d;
    }
    return Value();
}

[[maybe_unused]] Value getKwarg(vector<Value>& args, const string& key, Value def) {
    if (!args.empty() && args.back().kwargs && args.back().isDict()) {
        auto p = args.back().dict->find(Value(key));
        if (p) return *p;
    }
    return def;
}

void openWithSystem(const string& path) {
#ifdef _WIN32
    string cmd = "start \"\" \"" + path + "\"";
    system(cmd.c_str());
#elif __APPLE__
    string cmd = "open \"" + path + "\"";
    system(cmd.c_str());
#else
    string cmd = "xdg-open \"" + path + "\"";
    system(cmd.c_str());
#endif
}

bool zipFolder(const string& path, int level, bool removeAfter) {
    fs::path p = fs::absolute(path);
    if (!fs::exists(p)) return false;
    level = max(0, min(level, 9));
    string zipname = p.string() + ".zip";
    int err = 0;
    zip_t* za = zip_open(zipname.c_str(), ZIP_CREATE | ZIP_TRUNCATE, &err);
    if (!za) return false;

    auto addFile = [&](const fs::path& full, const string& arc) -> bool {
        zip_source_t* src = zip_source_file(za, full.string().c_str(), 0, 0);
        if (!src) return false;
        zip_int64_t idx = zip_file_add(za, arc.c_str(), src, ZIP_FL_OVERWRITE);
        if (idx < 0) {
            zip_source_free(src);
            return false;
        }
        zip_set_file_compression(za, idx, level == 0 ? ZIP_CM_STORE : ZIP_CM_DEFLATE, (zip_uint32_t)level);
        return true;
    };

    if (fs::is_regular_file(p)) {
        addFile(p, p.filename().string());
    } else {
        for (auto& e : fs::recursive_directory_iterator(p)) {
            if (e.is_regular_file()) {
                string arc = fs::relative(e.path(), p.parent_path()).generic_string();
                addFile(e.path(), arc);
            }
        }
    }

    zip_close(za);

    if (removeAfter) {
        error_code ec;
        fs::remove_all(p, ec);
    }
    return true;
}

struct CurlGlobalInit {
    CurlGlobalInit() { curl_global_init(CURL_GLOBAL_DEFAULT); }
    ~CurlGlobalInit() { curl_global_cleanup(); }
};
static CurlGlobalInit CURL_INIT;

static size_t curlWriteString(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto s = (string*)userdata;
    s->append(ptr, size * nmemb);
    return size * nmemb;
}

static size_t curlWriteFile(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto f = (ofstream*)userdata;
    f->write(ptr, (streamsize)(size * nmemb));
    return size * nmemb;
}

static size_t curlHeaderCallback(char* buffer, size_t size, size_t nitems, void* userdata) {
    auto headers = (vector<pair<string, string>>*)userdata;
    string line(buffer, size * nitems);
    size_t pos = line.find(':');
    if (pos != string::npos) {
        string k = trim(line.substr(0, pos));
        string v = trim(line.substr(pos + 1));
        if (!k.empty()) headers->push_back({k, v});
    }
    return size * nitems;
}


struct ParsedUrl {
    string scheme = "http";
    string host;
    string path = "/";
    int port = 80;
};

[[maybe_unused]] static ParsedUrl parseUrl(const string& url) {
    ParsedUrl u;
    size_t p = url.find("://");
    string rest;
    if (p != string::npos) {
        u.scheme = url.substr(0, p);
        rest = url.substr(p + 3);
    } else rest = url;
    size_t slash = rest.find('/');
    string hostport = slash == string::npos ? rest : rest.substr(0, slash);
    u.path = slash == string::npos ? "/" : rest.substr(slash);
    size_t colon = hostport.find(':');
    if (colon != string::npos) {
        u.host = hostport.substr(0, colon);
        u.port = stoi(hostport.substr(colon + 1));
    } else {
        u.host = hostport;
    }
    if (u.scheme == "https") u.port = 443;
    return u;
}

static Value doHttpRequest(const string& method,
                          const string& url,
                          const Value& headers,
                          const Value& data,
                          int timeout) {
    CURL* curl = curl_easy_init();
    if (!curl) throw ProstoError{"RuntimeError", "curl init failed"};

    string body;
    vector<pair<string, string>> hdrs;
    bool hasContentType = false;

    if (headers.isDict()) {
        for (auto& kv : headers.dict->items) {
            string k = kv.first.toStr();
            string v = kv.second.toStr();
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
                string q;
                for (auto& kv : data.dict->items) {
                    if (!q.empty()) q += "&";
                    q += urlEncodeString(kv.first.toStr()) + "=" + urlEncodeString(kv.second.toStr());
                }
                body = q;
            }
        } else {
            body = data.toStr();
        }
    }

    string responseBody;
    vector<pair<string, string>> responseHeaders;
    struct curl_slist* slist = nullptr;
    for (auto& h : hdrs) {
        slist = curl_slist_append(slist, (h.first + ": " + h.second).c_str());
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, (long)(timeout * 1000));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWriteString);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseBody);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, curlHeaderCallback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &responseHeaders);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    if (slist) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, slist);

    if (method == "POST") {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body.size());
    } else if (method != "GET") {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
        if (!body.empty()) {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body.size());
        }
    }

    CURLcode res = curl_easy_perform(curl);
    long status = 0;
    if (res == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    } else {
        responseBody = curl_easy_strerror(res);
    }

    if (slist) curl_slist_free_all(slist);
    curl_easy_cleanup(curl);

    return Value::makeNativeObject(make_shared<HttpResponseNative>(status, responseHeaders, responseBody));
}

static bool httpDownload(const string& url, const string& savePath, int timeout) {
    CURL* curl = curl_easy_init();
    if (!curl) return false;
    ofstream f(savePath, ios::binary);
    if (!f) {
        curl_easy_cleanup(curl);
        return false;
    }
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, (long)(timeout * 1000));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWriteFile);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &f);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    return res == CURLE_OK;
}


Value searchFiles(const string& pattern, const Value& recursive) {
    fs::path p = fs::absolute(pattern);
    vector<Value> out;
    if (!fs::exists(p)) return makeListFromVector(out);

    int maxDepth = 0;
    bool rec = false;
    if (recursive.type == VT::Bool) {
        rec = recursive.b;
        maxDepth = rec ? 9999 : 0;
    } else if (recursive.type == VT::Int) {
        rec = true;
        maxDepth = (int)recursive.i;
    }

    if (!rec) {
        out.push_back(Value::makeNativeObject(make_shared<EFCObject>(p.string())));
        return makeListFromVector(out);
    }

    function<void(const fs::path&, int)> walk = [&](const fs::path& p, int depth) {
        if (depth > maxDepth) return;
        if (fs::is_regular_file(p)) {
            out.push_back(Value::makeNativeObject(make_shared<EFCObject>(p.string())));
            return;
        }
        if (fs::is_directory(p)) {
            for (auto& e : fs::directory_iterator(p)) {
                if (fs::is_directory(e.path())) {
                    if (depth + 1 <= maxDepth) {
                        walk(e.path(), depth + 1);
                    }
                } else {
                    out.push_back(Value::makeNativeObject(make_shared<EFCObject>(e.path().string())));
                }
            }
        }
    };

    if (fs::is_directory(p)) {
        walk(p, 1);
    } else {
        out.push_back(Value::makeNativeObject(make_shared<EFCObject>(p.string())));
    }
    return makeListFromVector(out);
}


bool isDunder(const string& s) {
    return s.size() >= 4 && startsWith(s, "__") && endsWith(s, "__");
}

static string transformArrow(const string& expr) {
    char inStr = 0;
    int depth = 0;
    for (size_t i = 0; i + 1 < expr.size(); i++) {
        char c = expr[i];
        if (inStr) {
            if (c == inStr) inStr = 0;
            continue;
        }
        if (c == '"' || c == '\'') inStr = c;
        else if (c == '(' || c == '[' || c == '{') depth++;
        else if (c == ')' || c == ']' || c == '}') depth--;
        else if (c == '=' && expr[i + 1] == '>' && depth == 0) {
            string left = trim(expr.substr(0, i));
            string right = trim(expr.substr(i + 2));
            string params;
            if (!left.empty() && left.front() == '(' && left.back() == ')') {
                params = left.substr(1, left.size() - 2);
            } else {
                params = left;
            }
            return "lambda " + params + ": " + right;
        }
    }
    return expr;
}

struct ExprParser {
    string src;
    size_t pos = 0;
    Interpreter& interp;
    shared_ptr<Scope> scope;

    ExprParser(const string& s, Interpreter& in, shared_ptr<Scope> sc)
        : src(s), interp(in), scope(sc) {}

    void skip() {
        while (pos < src.size() && isspace((unsigned char)src[pos])) pos++;
    }

    bool matchOp(const string& op) {
        skip();
        if (src.compare(pos, op.size(), op) == 0) {
            pos += op.size();
            return true;
        }
        return false;
    }

    bool matchWord(const string& w) {
        skip();
        if (src.compare(pos, w.size(), w) == 0) {
            size_t n = pos + w.size();
            if (n >= src.size() || !(isalnum((unsigned char)src[n]) || src[n] == '_')) {
                pos = n;
                return true;
            }
        }
        return false;
    }

    void expectOp(const string& op) {
        if (!matchOp(op)) throw ProstoError{"SyntaxError", "expected '" + op + "'"};
    }

    void expectWord(const string& w) {
        if (!matchWord(w)) throw ProstoError{"SyntaxError", "expected '" + w + "'"};
    }

    bool tryParseName(string& out) {
        skip();
        size_t old = pos;
        if (pos >= src.size() || !(isalpha((unsigned char)src[pos]) || src[pos] == '_')) return false;
        string name;
        while (pos < src.size() && (isalnum((unsigned char)src[pos]) || src[pos] == '_')) {
            name += src[pos++];
        }
        out = name;
        (void)old;
        return true;
    }

    string parseName() {
        string name;
        if (!tryParseName(name)) throw ProstoError{"SyntaxError", "expected name"};
        if (isDunder(name)) throw SecurityError{"Blocked: '" + name + "'"};
        return name;
    }

    Value parseTop() {
        Value v = parseTernary();
        skip();
        if (pos < src.size() && src[pos] == ',') {
            vector<Value> items{v};
            while (matchOp(",")) {
                skip();
                if (pos >= src.size() || src[pos] == ')' || src[pos] == ']' || src[pos] == '}') break;
                items.push_back(parseTernary());
            }
            return makeListFromVector(items);
        }
        return v;
    }

    Value parseTernary() {
        Value val = parseOr();
        if (matchWord("if")) {
            Value cond = parseOr();
            expectWord("else");
            Value other = parseTernary();
            return cond.toBool() ? val : other;
        }
        return val;
    }

    Value parseOr() {
        Value v = parseAnd();
        while (matchWord("or")) {
            Value r = parseAnd();
            v = Value(v.toBool() || r.toBool());
        }
        return v;
    }

    Value parseAnd() {
        Value v = parseNot();
        while (matchWord("and")) {
            Value r = parseNot();
            v = Value(v.toBool() && r.toBool());
        }
        return v;
    }

    Value parseNot() {
        if (matchWord("not")) {
            Value v = parseNot();
            return Value(!v.toBool());
        }
        return parseComparison();
    }

    Value parseComparison() {
        Value first = parseBitOr();
        vector<pair<string, Value>> comps;

        while (true) {
            skip();
            string op;
            if (matchOp("==")) op = "==";
            else if (matchOp("!=")) op = "!=";
            else if (matchOp("<=")) op = "<=";
            else if (matchOp(">=")) op = ">=";
            else if (matchOp("<")) op = "<";
            else if (matchOp(">")) op = ">";
            else if (matchWord("in")) op = "in";
            else if (matchWord("not")) {
                expectWord("in");
                op = "not in";
            } else break;

            comps.push_back({op, parseBitOr()});
        }

        if (comps.empty()) return first;

        Value prev = first;
        bool ok = true;
        for (auto& c : comps) {
            bool r;
            if (c.first == "in") r = valueIn(prev, c.second);
            else if (c.first == "not in") r = !valueIn(prev, c.second);
            else r = compareValues(prev, c.first, c.second);
            if (!r) ok = false;
            prev = c.second;
        }
        return Value(ok);
    }

    Value parseBitOr() {
        Value v = parseBitXor();
        while (matchOp("|")) v = applyBinary("|", v, parseBitXor());
        return v;
    }

    Value parseBitXor() {
        Value v = parseBitAnd();
        while (matchOp("^")) v = applyBinary("^", v, parseBitAnd());
        return v;
    }

    Value parseBitAnd() {
        Value v = parseShift();
        while (matchOp("&")) v = applyBinary("&", v, parseShift());
        return v;
    }

    Value parseShift() {
        Value v = parseAdd();
        while (true) {
            if (matchOp("<<")) v = applyBinary("<<", v, parseAdd());
            else if (matchOp(">>")) v = applyBinary(">>", v, parseAdd());
            else break;
        }
        return v;
    }

    Value parseAdd() {
        Value v = parseTerm();
        while (true) {
            if (matchOp("+")) v = applyBinary("+", v, parseTerm());
            else if (matchOp("-")) v = applyBinary("-", v, parseTerm());
            else break;
        }
        return v;
    }

    Value parseTerm() {
        Value v = parseFactor();
        while (true) {
            if (matchOp("//")) v = applyBinary("//", v, parseFactor());
            else if (matchOp("*")) v = applyBinary("*", v, parseFactor());
            else if (matchOp("/")) v = applyBinary("/", v, parseFactor());
            else if (matchOp("%")) v = applyBinary("%", v, parseFactor());
            else break;
        }
        return v;
    }

    Value parseFactor() {
        if (matchOp("-")) {
            Value v = parseFactor();
            if (v.type == VT::Int) return Value(-v.i);
            return Value(-v.toFloat());
        }
        if (matchOp("+")) return parseFactor();
        return parsePower();
    }

    Value parsePower() {
        Value v = parsePostfix();
        if (matchOp("**")) {
            Value r = parseFactor();
            return applyBinary("**", v, r);
        }
        return v;
    }

    Value parsePostfix() {
        Value v = parsePrimary();

        while (true) {
            skip();
            if (matchOp(".")) {
                string name = parseName();
                v = interp.getAttr(v, name);
            } else if (matchOp("[")) {
                bool isSlice = false;
                Value start, end, step;
                skip();
                if (!matchOp(":")) {
                    start = parseTernary();
                }
                skip();
                if (matchOp(":")) {
                    isSlice = true;
                    skip();
                    if (pos < src.size() && src[pos] != ']' && src[pos] != ':') {
                        end = parseTernary();
                    }
                    skip();
                    if (matchOp(":")) {
                        step = parseTernary();
                    }
                }
                expectOp("]");
                if (isSlice) v = interp.sliceValue(v, start, end, step);
                else v = interp.indexValue(v, start);
            } else if (matchOp("(")) {
                vector<Value> args;
                vector<pair<Value, Value>> kwargs;
                skip();
                if (!matchOp(")")) {
                    while (true) {
                        skip();
                        size_t save = pos;
                        string id;
                        bool kw = false;
                        if (tryParseName(id)) {
                            skip();
                            if (pos < src.size() && src[pos] == '=' &&
                                !(pos + 1 < src.size() && src[pos + 1] == '=')) {
                                pos++;
                                kw = true;
                            }
                        }
                        if (kw) {
                            Value val = parseTernary();
                            kwargs.push_back({Value(id), val});
                        } else {
                            pos = save;
                            args.push_back(parseTernary());
                        }

                        skip();
                        if (matchOp(",")) continue;
                        expectOp(")");
                        break;
                    }
                }

                if (!kwargs.empty()) {
                    auto d = make_shared<Dict>();
                    for (auto& kv : kwargs) d->set(kv.first, kv.second);
                    Value kwv = Value::makeDict(d);
                    kwv.kwargs = true;
                    args.push_back(kwv);
                }

                v = interp.callValue(v, args, scope);
            } else break;
        }

        return v;
    }

    Value parsePrimary() {
        skip();

        if (pos >= src.size()) throw ProstoError{"SyntaxError", "unexpected end of expression"};

        char c = src[pos];

        if (isdigit((unsigned char)c) || (c == '.' && pos + 1 < src.size() && isdigit((unsigned char)src[pos + 1]))) {
            return parseNumber();
        }

        if (c == '"' || c == '\'') {
            return parseString();
        }

        if (c == '[') {
            pos++;
            vector<Value> items;
            skip();
            if (matchOp("]")) return makeListFromVector(items);
            while (true) {
                items.push_back(parseTernary());
                skip();
                if (matchOp(",")) {
                    skip();
                    if (matchOp("]")) break;
                    continue;
                }
                expectOp("]");
                break;
            }
            return makeListFromVector(items);
        }

        if (c == '{') {
            pos++;
            auto d = make_shared<Dict>();
            vector<Value> listItems;
            bool isDict = false;
            skip();
            if (matchOp("}")) return Value::makeDict(d);

            while (true) {
                Value key = parseTernary();
                skip();
                if (matchOp(":")) {
                    isDict = true;
                    Value val = parseTernary();
                    d->set(key, val);
                } else {
                    listItems.push_back(key);
                }
                skip();
                if (matchOp(",")) {
                    skip();
                    if (matchOp("}")) break;
                    continue;
                }
                expectOp("}");
                break;
            }

            if (isDict) return Value::makeDict(d);
            return makeListFromVector(listItems);
        }

        if (c == '(') {
            pos++;
            skip();
            if (matchOp(")")) return makeListFromVector({});
            Value first = parseTernary();
            skip();
            if (matchOp(",")) {
                vector<Value> items{first};
                while (true) {
                    skip();
                    if (matchOp(")")) break;
                    items.push_back(parseTernary());
                    skip();
                    if (matchOp(",")) continue;
                    expectOp(")");
                    break;
                }
                return makeListFromVector(items);
            }
            expectOp(")");
            return first;
        }

        string name;
        if (tryParseName(name)) {
            if (name == "true" || name == "True") return Value(true);
            if (name == "false" || name == "False") return Value(false);
            if (name == "None" || name == "null") return Value();

            if (name == "lambda") {
                vector<string> params;
                skip();
                if (pos < src.size() && src[pos] != ':') {
                    while (true) {
                        params.push_back(parseName());
                        skip();
                        if (matchOp(",")) continue;
                        break;
                    }
                }
                expectOp(":");
                skip();
                size_t bodyStart = pos;
                parseTernary();
                string body = trim(src.substr(bodyStart, pos - bodyStart));

                auto fn = make_shared<Function>();
                fn->native = true;
                fn->name = "<lambda>";
                fn->params = params;
                auto capturedScope = scope;
                fn->nativeFn = [params, body, capturedScope](Interpreter& in, vector<Value>& args) {
                    auto sc = make_shared<Scope>();
                    sc->parent = capturedScope;
                    for (size_t i = 0; i < params.size(); i++) {
                        sc->vars[params[i]] = (i < args.size()) ? args[i] : Value();
                    }
                    return in.evalExpr(body, sc);
                };
                return Value::makeFunction(fn);
            }

            if (isDunder(name)) throw SecurityError{"Blocked: '" + name + "'"};
            return interp.getVar(name, scope);
        }

        throw ProstoError{"SyntaxError", "unexpected character in expression"};
    }

    Value parseNumber() {
        skip();
        size_t start = pos;
        bool isFloat = false;

        if (src.compare(pos, 2, "0x") == 0 || src.compare(pos, 2, "0X") == 0) {
            pos += 2;
            while (pos < src.size() && isxdigit((unsigned char)src[pos])) pos++;
            string txt = src.substr(start, pos - start);
            return Value((long long)stoll(txt, nullptr, 16));
        }

        while (pos < src.size() && isdigit((unsigned char)src[pos])) pos++;
        if (pos < src.size() && src[pos] == '.') {
            isFloat = true;
            pos++;
            while (pos < src.size() && isdigit((unsigned char)src[pos])) pos++;
        }
        if (pos < src.size() && (src[pos] == 'e' || src[pos] == 'E')) {
            isFloat = true;
            pos++;
            if (pos < src.size() && (src[pos] == '+' || src[pos] == '-')) pos++;
            while (pos < src.size() && isdigit((unsigned char)src[pos])) pos++;
        }

        string txt = src.substr(start, pos - start);
        if (isFloat) return Value(stod(txt));
        return Value((long long)stoll(txt));
    }

    Value parseString() {
        skip();
        char quote = src[pos++];
        bool triple = false;
        if (pos + 1 < src.size() && src[pos] == quote && src[pos + 1] == quote) {
            triple = true;
            pos += 2;
        }

        string out;
        if (triple) {
            string end = string(3, quote);
            while (pos < src.size()) {
                if (src.compare(pos, 3, end) == 0) {
                    pos += 3;
                    return Value(out);
                }
                out += src[pos++];
            }
            throw ProstoError{"SyntaxError", "unterminated triple-quoted string"};
        }

        while (pos < src.size() && src[pos] != quote) {
            char c = src[pos++];
            if (c == '\\' && pos < src.size()) {
                char e = src[pos++];
                switch (e) {
                    case 'n': out += '\n'; break;
                    case 't': out += '\t'; break;
                    case 'r': out += '\r'; break;
                    case '\\': out += '\\'; break;
                    case '"': out += '"'; break;
                    case '\'': out += '\''; break;
                    case '0': out += '\0'; break;
                    default: out += e;
                }
            } else out += c;
        }

        if (pos >= src.size()) throw ProstoError{"SyntaxError", "unterminated string"};
        pos++;
        return Value(out);
    }
};
