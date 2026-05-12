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

// 统一的"读文件→解析→执行"辅助，消除 REPL 启动加载与批处理模式之间的重复。
static auto load_and_run(State &S, const std::string &path, bool verbose = false) -> Value {
    std::filesystem::path fpath(path);
    if (!std::filesystem::exists(fpath)) {
        if (verbose) std::cerr << "warning: file not found: " << path << "\n";
        return {};
    }
    std::ifstream f(fpath);
    if (!f) {
        if (verbose) std::cerr << "warning: failed to open file: " << path << "\n";
        return {};
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    Value parsed = S.parse_all(ss.str(), path);
    if (!parsed) return {};
    return S.do_list(parsed, S.global);
}

// 以“源码片段 + 光标”的形式打印调用链，主要服务宏展开与嵌套调用报错。
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
    // 优先使用 LispError 自带的位置；否则退回 current_expr 做近似定位。
    if (auto le = dynamic_cast<const LispError *>(&ex)) {
        if (le->has_loc) {
            print_error_with_loc(S, le->loc, le->what());
            if (!le->call_chain.empty())
                print_call_chain(S, le->call_chain);
            return;
        }
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
    std::string histfile;
    if (const char *home = getenv("HOME")) {
        histfile = std::string(home) + "/.VDLISP__history";
        read_history(histfile.c_str());
    }
    while (true) {
        char *cline = readline("> ");
        if (!cline) break;
        std::string line(cline);
        free(cline);
        if (line.empty()) continue;
        add_history(line.c_str());
        try {
            Value e = S.parse(line);
            if (e) std::cout << S.to_string(S.eval(e, S.global)) << "\n";
        } catch (const std::exception &ex) { report_exception(S, ex); }
    }
    if (!histfile.empty()) write_history(histfile.c_str());
}

// NaN-boxing 依赖 48 位 canonical pointer，这里在启动时做一次硬检查。
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
    // 正常退出时主动清理运行时，便于配合 ASAN/Valgrind 观察真实泄漏。
    struct ShutdownGuard {
        State &S;
        ~ShutdownGuard() {
            S.shutdown_and_purge_pools();
        }
    } guard{S};
    // 把宿主命令行参数暴露给 Lisp 世界。
    S.bind_global("argv", S.make_string_list(argc, argv, 1));
    // 如果提供了语言层辅助库，启动时自动加载。
    try {
        load_and_run(S, "scripts/lang_basics.lisp");
    } catch (const std::exception &ex) {
        std::cerr << "warning: failed to load startup helper script scripts/lang_basics.lisp: " << ex.what() << "\n";
    }
    if (argc < 2) {
        repl(S);
        return 0;
    }
    // 批处理模式：加载文件、解析顶层表达式并按顺序执行。
    try {
        Value r = load_and_run(S, argv[1]);
        std::cout << S.to_string(r) << "\n";
    } catch (const std::exception &ex) {
        report_exception(S, ex);
        return 1;
    }
    return 0;
}
