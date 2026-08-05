#include "prosto_common.hpp"

static void repl(Interpreter& interp) {
    cout << "Prosto+ v1.0.0 — Interactive REPL" << endl;
    cout << "Commands: exit | vars | funcs | help" << endl << endl;

    vector<string> buffer;
    int braceDepth = 0;

    while (true) {
        cout << (braceDepth > 0 ? "... " : "ptcp> ");
        cout.flush();

        string line;
        if (!getline(cin, line)) {
            cout << endl << "Bye!" << endl;
            break;
        }

        string stripped = trim(line);

        if (braceDepth == 0 && buffer.empty()) {
            if (stripped == "exit" || stripped == "quit") {
                cout << "Bye!" << endl;
                break;
            }
            if (stripped == "vars") {
                if (interp.globals.empty()) cout << "  (empty)" << endl;
                for (auto& kv : interp.globals) {
                    cout << "  " << kv.first << " = " << kv.second.repr() << endl;
                }
                continue;
            }
            if (stripped == "funcs") {
                if (interp.functions.empty()) cout << "  (empty)" << endl;
                for (auto& kv : interp.functions) {
                    cout << "  " << kv.first << "(";
                    for (size_t i = 0; i < kv.second->params.size(); i++) {
                        if (i) cout << ", ";
                        cout << kv.second->params[i];
                    }
                    cout << ")" << endl;
                }
                continue;
            }
            if (stripped == "help") {
                cout << "  Type Prosto+ code. Multi-line: use { }" << endl;
                cout << "  Commands: exit, vars, funcs, help" << endl;
                continue;
            }
            if (stripped.empty()) continue;
        }

        buffer.push_back(line);
        braceDepth += braceDelta(line);
        if (braceDepth > 0) continue;

        if (!buffer.empty()) {
            if (buffer.size() == 1) {
                string s = trim(buffer[0]);
                bool isExpr = !s.empty() && !startsWith(s, "#") && !startsWith(s, "--") &&
                              !regex_search(s, regex(R"(^[\w, ]+\s*=(?!=))")) &&
                              !startsWithSkipKeyword(s);
                if (isExpr) {
                    try {
                        Value r = interp.evalExpr(s, nullptr);
                        if (!r.isNull()) cout << r.repr() << endl;
                    } catch (ProstoError& e) {
                        interp.printError(e, 1);
                    } catch (SecurityError& e) {
                        cout << "Security Error: " << e.msg << endl;
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
                cout << "Security Error: " << e.msg << endl;
            } catch (...) {
                cout << "Error: unknown runtime error" << endl;
            }

            buffer.clear();
            braceDepth = 0;
        }
    }
}

int main(int argc, char** argv) {
    Interpreter interp;

    if (argc < 2) {
        repl(interp);
        return 0;
    }

    string script = argv[1];
    if (!endsWith(script, ".ptcp")) {
        cout << "Warning: File extension should be .ptcp" << endl;
    }

    try {
        auto lines = splitLines(readFileAll(script));
        interp.runLines(lines, nullptr, 1);
    } catch (ProstoError& e) {
        interp.printError(e, 0);
    } catch (SecurityError& e) {
        cout << "Security Error: " << e.msg << endl;
    } catch (exception& e) {
        cout << "Fatal Error: " << e.what() << endl;
    }

    return 0;
}
