#include "prosto_common.hpp"




// 补充缺失的函数声明
bool Value::operator<(const Value& other) const {
    // 实现比较逻辑
    if (isNumber() && other.isNumber()) {
        return toFloat() < other.toFloat();
    }
    // 其他类型的比较逻辑: compare strings, lists, dicts
    if (isString() && other.isString()) return s < other.s;
    // fallback: compare type enum and then string repr
    return static_cast<int>(type) < static_cast<int>(other.type);
}


Value Interpreter::getAttr(const Value& obj, const string& name) {
    if (isDunder(name)) throw SecurityError{"Blocked: '.__" + name + "'"};

    if (obj.isString()) {
        string s = obj.s;

        if (name == "upper") return Value::makeNativeFunction([s](Interpreter&, vector<Value>&) {
            return Value(upperString(s));
        }, "upper");

        if (name == "lower") return Value::makeNativeFunction([s](Interpreter&, vector<Value>&) {
            return Value(lowerString(s));
        }, "lower");

        if (name == "strip") return Value::makeNativeFunction([s](Interpreter&, vector<Value>&) {
            return Value(trim(s));
        }, "strip");

        if (name == "split") return Value::makeNativeFunction([s](Interpreter&, vector<Value>& args) {
            string sep;
            if (!args.empty() && !args[0].isNull()) sep = args[0].toStr();
            auto parts = splitString(s, sep);
            vector<Value> out;
            for (auto& p : parts) out.push_back(Value(p));
            return makeListFromVector(out);
        }, "split");

        if (name == "replace") return Value::makeNativeFunction([s](Interpreter&, vector<Value>& args) {
            if (args.size() < 2) throw ProstoError{"TypeError", "replace(old,new) requires 2 arguments"};
            string old = args[0].toStr();
            string neu = args[1].toStr();
            string out = s;
            size_t pos = 0;
            while ((pos = out.find(old, pos)) != string::npos) {
                out.replace(pos, old.size(), neu);
                pos += neu.size();
            }
            return Value(out);
        }, "replace");

        if (name == "find") return Value::makeNativeFunction([s](Interpreter&, vector<Value>& args) {
            if (args.empty()) throw ProstoError{"TypeError", "find(sub) requires 1 argument"};
            auto pos = s.find(args[0].toStr());
            return Value(pos == string::npos ? -1LL : (long long)pos);
        }, "find");

        if (name == "startswith") return Value::makeNativeFunction([s](Interpreter&, vector<Value>& args) {
            if (args.empty()) return Value(false);
            return Value(startsWith(s, args[0].toStr()));
        }, "startswith");

        if (name == "endswith") return Value::makeNativeFunction([s](Interpreter&, vector<Value>& args) {
            if (args.empty()) return Value(false);
            return Value(endsWith(s, args[0].toStr()));
        }, "endswith");

        if (name == "join") return Value::makeNativeFunction([s](Interpreter&, vector<Value>& args) {
            if (args.empty() || !args[0].isList()) throw ProstoError{"TypeError", "join(list) requires list"};
            string out;
            for (size_t i = 0; i < args[0].list->size(); i++) {
                if (i) out += s;
                out += (*args[0].list)[i].toStr();
            }
            return Value(out);
        }, "join");

        if (name == "format") return Value::makeNativeFunction([s](Interpreter&, vector<Value>& args) {
            return Value(formatString(s, args));
        }, "format");
    }

    if (obj.isList()) {
        auto l = obj.list;

        if (name == "append") return Value::makeNativeFunction([l](Interpreter&, vector<Value>& args) {
            if (!args.empty()) l->push_back(args[0]);
            return Value();
        }, "append");

        if (name == "pop") return Value::makeNativeFunction([l](Interpreter&, vector<Value>& args) {
            if (l->empty()) throw ProstoError{"IndexError", "pop from empty list"};
            long long idx = args.empty() ? -1 : args[0].toInt();
            if (idx < 0) idx += (long long)l->size();
            if (idx < 0 || idx >= (long long)l->size()) throw ProstoError{"IndexError", "pop index out of range"};
            Value v = (*l)[idx];
            l->erase(l->begin() + idx);
            return v;
        }, "pop");

        if (name == "insert") return Value::makeNativeFunction([l](Interpreter&, vector<Value>& args) {
            if (args.size() < 2) throw ProstoError{"TypeError", "insert(index,value) requires 2 arguments"};
            long long idx = args[0].toInt();
            if (idx < 0) idx += (long long)l->size();
            if (idx < 0) idx = 0;
            if (idx > (long long)l->size()) idx = (long long)l->size();
            l->insert(l->begin() + idx, args[1]);
            return Value();
        }, "insert");

        if (name == "remove") return Value::makeNativeFunction([l](Interpreter&, vector<Value>& args) {
            if (args.empty()) throw ProstoError{"TypeError", "remove(value) requires 1 argument"};
            for (size_t i = 0; i < l->size(); i++) {
                if (Value::equals((*l)[i], args[0])) {
                    l->erase(l->begin() + i);
                    return Value();
                }
            }
            throw ProstoError{"ValueError", "list.remove(x): x not in list"};
        }, "remove");

        if (name == "clear") return Value::makeNativeFunction([l](Interpreter&, vector<Value>&) {
            l->clear();
            return Value();
        }, "clear");

        if (name == "index") return Value::makeNativeFunction([l](Interpreter&, vector<Value>& args) {
            if (args.empty()) throw ProstoError{"TypeError", "index(value) requires 1 argument"};
            for (size_t i = 0; i < l->size(); i++) {
                if (Value::equals((*l)[i], args[0])) return Value((long long)i);
            }
            throw ProstoError{"ValueError", "value not in list"};
        }, "index");

        if (name == "count") return Value::makeNativeFunction([l](Interpreter&, vector<Value>& args) {
            if (args.empty()) return Value(0LL);
            long long c = 0;
            for (auto& x : *l) if (Value::equals(x, args[0])) c++;
            return Value(c);
        }, "count");

        if (name == "reverse") return Value::makeNativeFunction([l](Interpreter&, vector<Value>&) {
            reverse(l->begin(), l->end());
            return Value();
        }, "reverse");

        if (name == "sort") return Value::makeNativeFunction([l](Interpreter&, vector<Value>& args) {
            bool rev = !args.empty() && args[0].toBool();
            if (rev) sort(l->begin(), l->end(), [](const Value& a, const Value& b) { return lessValue(b, a); });
            else sort(l->begin(), l->end(), lessValue);
            return Value();
        }, "sort");

        if (name == "extend") return Value::makeNativeFunction([l](Interpreter&, vector<Value>& args) {
            if (!args.empty() && args[0].isList()) {
                for (auto& x : *args[0].list) l->push_back(x);
            }
            return Value();
        }, "extend");
    }

    if (obj.isDict()) {
        auto d = obj.dict;

        if (name == "keys") return Value::makeNativeFunction([d](Interpreter&, vector<Value>&) {
            vector<Value> out;
            for (auto& kv : d->items) out.push_back(kv.first);
            return makeListFromVector(out);
        }, "keys");

        if (name == "values") return Value::makeNativeFunction([d](Interpreter&, vector<Value>&) {
            vector<Value> out;
            for (auto& kv : d->items) out.push_back(kv.second);
            return makeListFromVector(out);
        }, "values");

        if (name == "items") return Value::makeNativeFunction([d](Interpreter&, vector<Value>&) {
            vector<Value> out;
            for (auto& kv : d->items) {
                out.push_back(makeListFromVector({kv.first, kv.second}));
            }
            return makeListFromVector(out);
        }, "items");

        if (name == "get") return Value::makeNativeFunction([d](Interpreter&, vector<Value>& args) {
            if (args.empty()) return Value();
            Value def = args.size() > 1 ? args[1] : Value();
            return d->get(args[0], def);
        }, "get");

        if (name == "pop") return Value::makeNativeFunction([d](Interpreter&, vector<Value>& args) {
            if (args.empty()) throw ProstoError{"TypeError", "pop(key) requires 1 argument"};
            auto p = d->find(args[0]);
            if (!p) throw ProstoError{"KeyError", args[0].repr()};
            Value v = *p;
            d->erase(args[0]);
            return v;
        }, "pop");

        if (name == "update") return Value::makeNativeFunction([d](Interpreter&, vector<Value>& args) {
            if (!args.empty() && args[0].isDict()) {
                for (auto& kv : args[0].dict->items) d->set(kv.first, kv.second);
            }
            return Value();
        }, "update");

        if (name == "clear") return Value::makeNativeFunction([d](Interpreter&, vector<Value>&) {
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
                return Value::makeNativeFunction([o, func](Interpreter& in, vector<Value>& args) {
                    vector<Value> all;
                    all.push_back(Value::makeObject(o));
                    for (auto& a : args) all.push_back(a);
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


Value Interpreter::indexValue(const Value& obj, const Value& idx) {
    if (obj.isList()) {
        long long i = idx.toInt();
        long long n = (long long)obj.list->size();
        if (i < 0) i += n;
        if (i < 0 || i >= n) throw ProstoError{"IndexError", "list index out of range"};
        return (*obj.list)[i];
    }

    if (obj.isString()) {
        long long i = idx.toInt();
        long long n = (long long)obj.s.size();
        if (i < 0) i += n;
        if (i < 0 || i >= n) throw ProstoError{"IndexError", "string index out of range"};
        return Value(string(1, obj.s[(size_t)i]));
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
        vector<Value> out;
        if (st > 0) {
            for (long long i = a; i < b; i += st) out.push_back((*obj.list)[i]);
        } else {
            for (long long i = a; i > b; i += st) out.push_back((*obj.list)[i]);
        }
        return makeListFromVector(out);
    }

    if (obj.isString()) {
        long long n = (long long)obj.s.size();
        long long a = start.isNull() ? (st > 0 ? 0 : n - 1) : normalize(start.toInt(), n, 0);
        long long b = end.isNull() ? (st > 0 ? n : -1) : normalize(end.toInt(), n, n);
        string out;
        if (st > 0) {
            for (long long i = a; i < b; i += st) out += obj.s[(size_t)i];
        } else {
            for (long long i = a; i > b; i += st) out += obj.s[(size_t)i];
        }
        return Value(out);
    }

    throw ProstoError{"TypeError", "value is not sliceable"};
}


int braceDelta(const string& line) {
    int d = 0;
    char q = 0;
    for (size_t i = 0; i < line.size(); i++) {
        char c = line[i];
        if (q) {
            if (c == '\\') {
                i++;
                continue;
            }
            if (c == q) q = 0;
            continue;
        }
        if (c == '"' || c == '\'') {
            q = c;
            continue;
        }
        if (c == '#') break;
        if (c == '{') d++;
        else if (c == '}') d--;
    }
    return d;
}

static pair<vector<string>, size_t> getBraceBlock(const vector<string>& lines, size_t start) {
    size_t i = start;
    while (i < lines.size() && lines[i].find('{') == string::npos) i++;
    if (i >= lines.size()) return {{}, lines.size()};

    string collected;
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
    string inner;
    if (first != string::npos && last != string::npos && last > first) {
        inner = collected.substr(first + 1, last - first - 1);
    }

    vector<string> out;
    if (!trim(inner).empty()) out = splitLines(inner);
    return {out, j + 1};
}

static vector<string> parseParams(const string& s) {
    vector<string> out;
    for (auto& p : splitString(s, ",")) {
        string x = trim(p);
        if (!x.empty()) out.push_back(x);
    }
    return out;
}

static bool matchKeywordExpr(const string& line, const string& kw, string& expr) {
    try {
        regex re1("^" + kw + "\\s*\\((.+)\\)\\s*\\{?$");
        regex re2("^" + kw + "\\s+(.+?)\\s*\\{$");
        smatch m;
        if (regex_search(line, m, re1)) {
            expr = m[1].str();
            return true;
        }
        if (regex_search(line, m, re2)) {
            expr = m[1].str();
            return true;
        }
    } catch (...) {}
    return false;
}

static vector<string> SKIP_KEYWORDS = {
    "if", "elif", "else", "while", "def", "try", "except",
    "circulate", "multithreading", "multiprocess", "print",
    "return", "break", "continue", "global", "switch", "class",
    "case", "default", "import_pkg", "import"
};

bool startsWithSkipKeyword(const string& s) {
    for (auto& k : SKIP_KEYWORDS) {
        if (s == k || startsWith(s, k + " ") || startsWith(s, k + "{") || startsWith(s, k + "(")) {
            return true;
        }
    }
    return false;
}

static optional<vector<Value>> parseSequence(Interpreter& interp, const string& expr, shared_ptr<Scope> scope) {
    regex rangeRe(R"(^(-?\d+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)$)");
    smatch m;
    string e = trim(expr);
    if (regex_match(e, m, rangeRe)) {
        long long a = stoll(m[1].str());
        long long b = stoll(m[2].str());
        long long c = stoll(m[3].str());
        vector<Value> out;
        if (c > 0) for (long long x = a; x <= b; x += c) out.push_back(Value(x));
        if (c < 0) for (long long x = a; x >= b; x += c) out.push_back(Value(x));
        return out;
    }

    Value v = interp.evalExpr(e, scope);
    if (v.isList()) {
        return *v.list;
    }
    return nullopt;
}

static vector<string> parseExceptTypes(const string& line) {
    vector<string> out;
    regex re(R"(^except\s*(?:\(([^)]*)\))?)");
    smatch m;
    if (regex_search(line, m, re) && m.size() > 1 && m[1].matched) {
        for (auto& t : splitString(m[1].str(), ",")) {
            string x = trim(t);
            if (!x.empty()) out.push_back(x);
        }
    }
    return out;
}

static bool exceptionMatches(const string& type, const vector<string>& allowed) {
    if (allowed.empty()) return true;
    for (auto& a : allowed) {
        if (a == type || a == "Exception") return true;
    }
    return false;
}

void Interpreter::runLines(const vector<string>& lines, shared_ptr<Scope> scope, int baseLine) {
    size_t i = 0;
    bool inMultilineComment = false;

    while (i < lines.size()) {
        string line = lines[i];
        if (!line.empty() && line.back() == '\n') line.pop_back();
        string stripped = trim(line);
        int ln = baseLine + (int)i;

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
            string expr = trim(stripped.substr(6));
            if (expr.empty()) throw ReturnSignal{Value()};
            throw ReturnSignal{evalExpr(expr, scope)};
        }

        if (stripped == "break") throw BreakSignal{};
        if (stripped == "continue") throw ContinueSignal{};

        smatch m;

        if (regex_search(stripped, m, regex(R"(^global\s+([\w, ]+))"))) {
            for (auto& n : splitString(m[1].str(), ",")) {
                string x = trim(n);
                if (!x.empty() && scope) scope->globalNames.insert(x);
            }
            i++;
            continue;
        }

        if (regex_search(stripped, m, regex(R"(^import\s*<\s*([^>]+)\s*>)"))) {
            string fname = trim(m[1].str());
            if (!importedFiles.count(fname)) {
                importedFiles.insert(fname);
                string target = fname;
                if (!fs::exists(target) && endsWith(target, ".ptc")) target += "p";
                if (!fs::exists(target) && !endsWith(target, ".ptcp")) target += ".ptcp";

                if (fs::exists(target)) {
                    auto content = splitLines(readFileAll(target));
                    runLines(content, scope, 1);
                } else {
                    cout << "Error [line " << ln << "]: File '" << fname << "' not found." << endl;
                }
            }
            i++;
            continue;
        }

        if (regex_search(stripped, m, regex(R"(^import_pkg\s+(\w[\w-]*))"))) {
            importPackage(m[1].str(), scope, ln);
            i++;
            continue;
        }

        if (!startsWithSkipKeyword(stripped)) {
            if (regex_search(stripped, m, regex(R"(^([A-Za-z_]\w*(?:\.[A-Za-z_]\w*)*)\.([A-Za-z_]\w*)\s*=(?!=)\s*(.+)$)"))) {
                string objExpr = m[1].str();
                string attr = m[2].str();
                string rhs = m[3].str();

                vector<string> parts = splitString(objExpr, ".");
                Value obj = getVar(parts[0], scope);
                for (size_t k = 1; k < parts.size(); k++) {
                    obj = getAttr(obj, parts[k]);
                }
                setAttr(obj, attr, evalExpr(rhs, scope));
                i++;
                continue;
            }

            if (regex_search(stripped, m, regex(R"(^([A-Za-z_]\w*)\s*(\+=|-=|\*=|/=|%=|\*\*=|//=)\s*(.+)$)"))) {
                string var = m[1].str();
                string op = m[2].str();
                string rhs = m[3].str();
                Value old = getVar(var, scope);
                Value rv = evalExpr(rhs, scope);
                string binop = op.substr(0, op.size() - 1);
                assignVar(var, applyBinary(binop, old, rv), scope);
                i++;
                continue;
            }

            if (regex_search(stripped, m, regex(R"(^([A-Za-z_][\w, ]*)\s*=(?!=)\s*(.+)$)"))) {
                vector<string> names;
                for (auto& n : splitString(m[1].str(), ",")) {
                    string x = trim(n);
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

        if (regex_search(stripped, m, regex(R"(^class\s+(\w+)\s*\{?)"))) {
            string cname = m[1].str();
            auto [block, jump] = getBraceBlock(lines, i);

            auto cls = make_shared<Class>();
            cls->name = cname;

            size_t j = 0;
            while (j < block.size()) {
                string bl = trim(block[j]);
                smatch mm;
                if (regex_search(bl, mm, regex(R"(^def\s+(\w+)\s*\((.*?)\)\s*\{?)"))) {
                    string mn = mm[1].str();
                    vector<string> mp = parseParams(mm[2].str());
                    auto [mb, mj] = getBraceBlock(block, j);
                    if (mn == "init") mn = "__init__";

                    auto fn = make_shared<Function>();
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

            classes[cname] = cls;
            {
                lock_guard<recursive_mutex> lock(globalMutex);
                globals[cname] = Value::makeClass(cls);
            }
            i = jump;
            continue;
        }

        if (regex_search(stripped, m, regex(R"(^switch\s*\((.+)\)\s*\{?)"))) {
            string expr = m[1].str();
            auto [block, jump] = getBraceBlock(lines, i);
            Value sv = evalExpr(expr, scope);

            vector<pair<string, vector<string>>> cases;
            string currentVal;
            vector<string> currentBlock;
            bool inCase = false;

            for (auto& raw : block) {
                string s = trim(raw);
                smatch cm;
                if (regex_search(s, cm, regex(R"(^case\s+(.+?)\s*:)"))) {
                    if (inCase) cases.push_back({currentVal, currentBlock});
                    currentVal = cm[1].str();
                    currentBlock = {};
                    inCase = true;
                } else if (regex_search(s, regex(R"(^default\s*:)"))) {
                    if (inCase) cases.push_back({currentVal, currentBlock});
                    currentVal = "__default__";
                    currentBlock = {};
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

        string condExpr;
        if (matchKeywordExpr(stripped, "if", condExpr)) {
            auto [block, jump] = getBraceBlock(lines, i);
            if (evalExpr(condExpr, scope).toBool()) {
                runLines(block, scope, ln + 1);
                i = jump;
                while (i < lines.size() &&
                       (startsWith(trim(lines[i]), "elif") || startsWith(trim(lines[i]), "else"))) {
                    auto [b2, j2] = getBraceBlock(lines, i);
                    i = j2;
                }
            } else {
                i = jump;
                bool handled = false;
                while (i < lines.size() && startsWith(trim(lines[i]), "elif")) {
                    string eexpr;
                    if (matchKeywordExpr(trim(lines[i]), "elif", eexpr)) {
                        auto [b2, j2] = getBraceBlock(lines, i);
                        if (evalExpr(eexpr, scope).toBool()) {
                            runLines(b2, scope, ln + 1);
                            i = j2;
                            handled = true;
                            while (i < lines.size() &&
                                   (startsWith(trim(lines[i]), "elif") || startsWith(trim(lines[i]), "else"))) {
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

        if (regex_search(stripped, m, regex(R"(^def\s+(\w+)\s*\((.*?)\)\s*\{?)"))) {
            string fname = m[1].str();
            vector<string> params = parseParams(m[2].str());
            auto [block, jump] = getBraceBlock(lines, i);

            auto fn = make_shared<Function>();
            fn->name = fname;
            fn->params = params;
            fn->block = block;
            functions[fname] = fn;

            i = jump;
            continue;
        }

        if (regex_search(stripped, m, regex(R"(^circulate\s*:\s*(\w+)\s*\((.+)\)\s*\{?)"))) {
            string var = m[1].str();
            string expr = m[2].str();
            auto [block, jump] = getBraceBlock(lines, i);
            auto seq = parseSequence(*this, expr, scope);
            if (seq) {
                for (auto& item : *seq) {
                    auto child = make_shared<Scope>();
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

        if (regex_search(stripped, m, regex(R"(^multithreading\s*:\s*(\w+)\s*\((.+)\)\s*\{?)"))) {
            string var = m[1].str();
            string expr = m[2].str();
            auto [block, jump] = getBraceBlock(lines, i);
            auto seq = parseSequence(*this, expr, scope);
            if (seq) {
                vector<thread> threads;
                for (auto& item : *seq) {
                    auto child = make_shared<Scope>();
                    child->parent = scope;
                    child->vars[var] = item;
                    threads.emplace_back([this, block, child, ln]() {
                        try {
                            runLines(block, child, ln + 1);
                        } catch (ProstoError& e) {
                            cout << "Thread Error: " << e.msg << endl;
                        } catch (...) {}
                    });
                }
                for (auto& t : threads) t.join();
            }
            i = jump;
            continue;
        }

        if (regex_search(stripped, m, regex(R"(^multiprocess\s*:\s*(\w+)\s*\((.+)\)\s*\{?)"))) {
            string var = m[1].str();
            string expr = m[2].str();
            auto [block, jump] = getBraceBlock(lines, i);
            auto seq = parseSequence(*this, expr, scope);
            if (seq) {
                static bool warned = false;
                if (!warned) {
                    cout << "Warning [line " << ln << "]: multiprocess uses threads on this platform" << endl;
                    warned = true;
                }

                vector<thread> threads;
                for (auto& item : *seq) {
                    auto child = make_shared<Scope>();
                    child->parent = scope;
                    child->vars[var] = item;
                    threads.emplace_back([this, block, child, ln]() {
                        try {
                            runLines(block, child, ln + 1);
                        } catch (ProstoError& e) {
                            cout << "Process Error: " << e.msg << endl;
                        } catch (...) {}
                    });
                }
                for (auto& t : threads) t.join();
            }
            i = jump;
            continue;
        }

        if (regex_search(stripped, m, regex(R"(^print\s*\((.*)\)$)"))) {
            string argsStr = m[1].str();
            string endVal = "\n";

            regex endRe(R"((?:^|,)\s*end\s*=\s*("[^"]*"|'[^']*'))");
            smatch em;
            string contentStr = argsStr;
            if (regex_search(argsStr, em, endRe)) {
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
                            cout << x.toStr() << endVal;
                        }
                    } else {
                        cout << val.toStr() << endVal;
                    }
                } catch (...) {
                    cout << contentStr << endVal;
                }
            } else {
                cout << endVal;
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
