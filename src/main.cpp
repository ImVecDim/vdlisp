#include "helpers.hpp"
#include "vdlisp.hpp"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <readline/history.h>
#include <readline/readline.h>
#include <sstream>

using namespace vdlisp;

namespace {

static void print_call_chain(const State &S, const std::vector<State::SourceLoc> &chain) {
    if (chain.empty())
        return;
    std::cerr << "Call chain:\n";
    for (const auto &fr : chain) {
        std::cerr << "  at ";
        if (!fr.label.empty())
            std::cerr << fr.label << " ";
        std::cerr << fr.file << ":" << fr.line << ":" << fr.col << "\n";
        std::string line;
        if (S.get_source_line(fr.file, fr.line, line)) {
            std::cerr << "    " << line << "\n";
            size_t col_index = fr.col ? fr.col - 1 : 0;
            std::string caret_spaces;
            for (size_t i = 0; i < col_index; ++i)
                caret_spaces.push_back((i < line.size() && line[i] == '\t') ? '\t' : ' ');
            std::cerr << "    " << caret_spaces << "^" << "\n";
        }
    }
}

static void report_exception(State &S, const std::exception &ex) {
    if (auto pe = dynamic_cast<const ParseError *>(&ex)) {
        print_error_with_loc(S, pe->loc, pe->what());
        if (!pe->call_chain.empty())
            print_call_chain(S, pe->call_chain);
        return;
    }
    State::SourceLoc loc;
    bool have_loc = S.get_source_loc(S.current_expr, loc);
    if (have_loc) {
        print_error_with_loc(S, loc, ex.what());
        auto it = S.src_call_chain_map.find(S.current_expr.identity_key());
        if (it != S.src_call_chain_map.end()) {
            print_call_chain(S, it->second);
        }
    } else {
        std::cerr << "error: " << ex.what() << "\n";
    }
}

static void repl(State &S) {
    const char *home = getenv("HOME");
    std::string histfile;
    if (home)
        histfile = std::string(home) + "/.VDLISP__history";

    if (!histfile.empty())
        read_history(histfile.c_str());

    while (true) {
        char *cline = readline("> ");
        if (!cline)
            break; // EOF (Ctrl-D)
        std::string line(cline);
        free(cline);
        if (line.empty())
            continue;
        add_history(line.c_str());
        try {
            Value e = S.parse(line);
            if (!e)
                continue;
            Value r = S.eval(e, S.global);
            std::cout << S.to_string(r) << "\n";
        } catch (const std::exception &ex) {
            report_exception(S, ex);
        }
    }

    if (!histfile.empty())
        write_history(histfile.c_str());
}

// Check at runtime that NaN-boxing assumptions hold on this platform.
static auto check_nanboxing_environment() -> bool {
    void *p = ::operator new(1);
    auto addr = reinterpret_cast<uint64_t>(p);
    ::operator delete(p);
    if ((addr & ~vdlisp::Value::kPayloadMask) != 0)
        return false;
    return true;
}

} // namespace

auto main(int argc, char **argv) -> int {
    if (!check_nanboxing_environment()) {
        std::cerr << "vdlisp: unsupported platform for NaN-boxing: pointers require more than 48 bits.\n"
                  << "This build assumes canonical 48-bit virtual addresses (x86_64)." << std::endl;
        return 1;
    }

    State S;
    // Ensure we return pooled memory on normal exit (helps leak checkers).
    struct ShutdownGuard {
        State &S;
        ~ShutdownGuard() {
            S.shutdown_and_purge_pools();
        }
    } guard{S};
    // bind argv as a list of strings into the global environment
    S.bind_global("argv", S.make_string_list(argc, argv, 1));
    // Auto-load core language helpers implemented in Lisp if supplied.
    try {
        std::filesystem::path langfile("scripts/lang_basics.lisp");
        if (std::filesystem::exists(langfile)) {
            std::ifstream lf(langfile);
            if (lf) {
                std::ostringstream lss;
                lss << lf.rdbuf();
                Value le = S.parse_all(lss.str(), langfile.string());
                if (le)
                    (void)S.do_list(le, S.global);
            }
        }
    } catch (...) {
        // ignore failures to auto-load language file
    }
    if (argc < 2) {
        repl(S);
        return 0;
    }
    // Load and execute file
    try {
        std::ifstream f(argv[1]);
        if (!f) {
            std::cerr << "could not open file: " << argv[1] << "\n";
            return 1;
        }
        std::ostringstream ss;
        ss << f.rdbuf();
        Value e = S.parse_all(ss.str(), argv[1]);
        if (e) {
            Value r = S.do_list(e, S.global);
            std::cout << S.to_string(r) << "\n";
        }
    } catch (const std::exception &ex) {
        report_exception(S, ex);
        return 1;
    }
    return 0;
}
