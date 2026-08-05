#include "prosto/interpreter.hpp"
#include "prosto/utils_decl.hpp"

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <memory>
#include <regex>

namespace prosto {

static int braceDelta(const std::string& line) {
    int delta = 0;
    bool inSingle = false, inDouble = false;
    for (size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (c == '\\') { // escape
            ++i;
            continue;
        }
        if (!inSingle && c == '"') inDouble = !inDouble;
        else if (!inDouble && c == '\'') inSingle = !inSingle;
        if (inSingle || inDouble) continue;
        if (c == '{') ++delta;
        else if (c == '}') --delta;
    }
    return delta;
}

static bool startsWithSkipKeyword(const std::string& s) {
    // Basic heuristics: if the first token is a control/definition keyword, treat as statement
    static const std::vector<std::string> keywords = {
        "if","for","while","class","def","return","import","from",
        "try","with","switch","case","break","continue","else","elif",
        "match","namespace","var","let","const","throw"};
    size_t i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    size_t j = i;
    while (j < s.size() && !std::isspace(static_cast<unsigned char>(s[j])) && s[j] != '(') ++j;
    if (j <= i) return false;
    std::string tok = s.substr(i, j - i);
    for (auto& c : tok) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (auto& kw : keywords) if (tok == kw) return true;
    return false;
}

void repl(Interpreter& interp) {
    std::cout << "Prosto+ v1.0.0 — Interactive REPL\n";
    std::cout << "Commands: exit | vars | funcs | help\n\n";

    std::vector<std::string> buffer;
    int braceDepth = 0;

    while (true) {
        std::cout << (braceDepth > 0 ? "... " : "ptcp> ");
        std::cout.flush();

        std::string line;
        if (!std::getline(std::cin, line)) {
            std::cout << "\nBye!\n";
            break;
        }

        std::string stripped = trim(line);

        if (braceDepth == 0 && buffer.empty()) {
            if (stripped == "exit" || stripped == "quit") {
                std::cout << "Bye!\n";
                break;
            }
            if (stripped == "vars") {
                auto g = interp.globals();
                if (g.empty()) std::cout << "  (empty)\n";
                for (auto& kv : g)
                    std::cout << "  " << kv.first << " = " << kv.second.repr() << "\n";
                continue;
            }
            if (stripped == "funcs") {
                auto f = interp.functions();
                if (f.empty()) std::cout << "  (empty)\n";
                for (auto& kv : f) {
                    std::cout << "  " << kv.first << "(";
                    for (size_t i = 0; i < kv.second->params.size(); i++) {
                        if (i) std::cout << ", ";
                        std::cout << kv.second->params[i];
                    }
                    std::cout << ")\n";
                }
                continue;
            }
            if (stripped == "help") {
                std::cout << "  Type Prosto+ code. Multi-line: use { }\n";
                std::cout << "  Commands: exit, vars, funcs, help\n";
                continue;
            }
            if (stripped.empty()) continue;
        }

        buffer.push_back(line);
        braceDepth += braceDelta(line);
        if (braceDepth > 0) continue;

        if (!buffer.empty()) {
            if (buffer.size() == 1) {
                std::string s = trim(buffer[0]);
                bool isExpr = !s.empty() && !startsWith(s, "#") && !startsWith(s, "--") &&
                              !std::regex_search(s, std::regex(R"(^[\w, ]+\s*=(?!=))")) &&
                              !startsWithSkipKeyword(s);
                if (isExpr) {
                    try {
                        Value r = interp.evalExpr(s, nullptr);
                        if (!r.isNull()) std::cout << r.repr() << "\n";
                    } catch (ProstoError& e) {
                        interp.printError(e, 1);
                    } catch (SecurityError& e) {
                        std::cout << "Security Error: " << e.msg << "\n";
                    }
                    buffer.clear();
                    braceDepth = 0;
                    continue;
                }
            }

            try {
                interp.runLines(buffer, nullptr, 1);
            } catch (ProstoError& e) {
                interp.printError(e, 1);
            } catch (SecurityError& e) {
                std::cout << "Security Error: " << e.msg << "\n";
            } catch (...) {
                std::cout << "Error: unknown runtime error\n";
            }

            buffer.clear();
            braceDepth = 0;
        }
    }
}

} // namespace prosto

int main(int argc, char** argv) {
    prosto::Interpreter interp;

    if (argc < 2) {
        prosto::repl(interp);
        return 0;
    }

    std::string script = argv[1];
    if (!prosto::endsWith(script, ".ptcp"))
        std::cout << "Warning: File extension should be .ptcp\n";

    try {
        auto lines = prosto::splitLines(prosto::readFileAll(script));
        interp.runLines(lines, nullptr, 1);
    } catch (prosto::ProstoError& e) {
        interp.printError(e, 0);
    } catch (prosto::SecurityError& e) {
        std::cout << "Security Error: " << e.msg << "\n";
    } catch (std::exception& e) {
        std::cout << "Fatal Error: " << e.what() << "\n";
    }

    return 0;
}
