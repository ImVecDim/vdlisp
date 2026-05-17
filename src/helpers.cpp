#include "state.hpp"
#include <charconv>
#include <cstdlib>
#include <iostream>
#include <unistd.h>

namespace vdlisp {

// Lisp 读取器里的分隔符集合：一旦遇到这些字符，当前 token 就结束。
// 用编译期查表替代运行时多分支判断，消除解析热路径上的分支预测开销。
static constexpr auto kDelim = [] {
    std::array<bool, 256> t{};
    t['('] = t[')'] = t['\''] = t['"'] = t[';'] = t['`'] = t[','] = true;
    // std::isspace 在 MinGW 上不是 constexpr，手动列空白字符
    t[' '] = t['\t'] = t['\n'] = t['\r'] = t['\v'] = t['\f'] = true;
    return t;
}();
[[gnu::always_inline]] inline static auto is_delim(char c) noexcept -> bool {
    return kDelim[static_cast<unsigned char>(c)];
}

// string_view 游标：消费时 remove_prefix，避免 src+pos 双参数
static void advance(std::string_view &cur, size_t &line, size_t &col) noexcept {
    if (cur.empty()) return;
    char c = cur.front();
    cur.remove_prefix(1);
    if (c == '\n') { ++line; col = 1; }
    else { ++col; }
}

static void skip_ws_and_comments(std::string_view &cur, size_t &line, size_t &col) noexcept {
    while (!cur.empty()) {
        char c = cur.front();
        // 批跳过水平空白字符 (space, tab, cr, vt, ff)，避免逐字节 remove_prefix
        if (c == ' ' || c == '\t' || c == '\r' || c == '\v' || c == '\f') {
            size_t n = 1;
            const char *p = cur.data();
            const char *end_ptr = p + cur.size();
            while (p + n < end_ptr) {
                char ch = p[n];
                if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\v' || ch == '\f')
                    ++n;
                else
                    break;
            }
            col += n;
            cur.remove_prefix(n);
            continue;
        }
        if (c == '\n') {
            cur.remove_prefix(1);
            ++line; col = 1;
            continue;
        }
        if (c == ';') {
            auto nl = cur.find('\n');
            if (nl == std::string_view::npos) {
                cur = {};
            } else {
                ++line;
                col = 1;
                cur.remove_prefix(nl + 1);
            }
            continue;
        }
        break;
    }
}

// quote/quasiquote/unquote 的解析完全相同，只有符号名不同
static auto parse_at(State &S, std::string_view &cur, size_t &line, size_t &col, const std::string &name) -> Value;
static auto parse_quoted(State &S, std::string_view &cur, size_t &line, size_t &col, const std::string &name, const char *keyword) -> Value {
    size_t qline = line;
    size_t qcol = col;
    advance(cur, line, col);
    Value inner = parse_at(S, cur, line, col, name);
    Value res = list_of(S, S.make_symbol(keyword), std::move(inner));
    S.set_source_loc(res, name, qline, qcol);
    return res;
}


static auto parse_at(State &S, std::string_view &cur, size_t &line, size_t &col, const std::string &name) -> Value {
    skip_ws_and_comments(cur, line, col);
    if (cur.empty()) [[unlikely]]
        return {};
    char c = cur.front();
    if (c == ')') {
        throw LispError(SourceLoc{name, line, col}, "unexpected )");
    }
    if (c == '(') {
        size_t open_line = line;
        size_t open_col = col;

        advance(cur, line, col);
        ListBuilder lb;
        bool closed = false;
        while (true) {
            skip_ws_and_comments(cur, line, col);
            if (cur.empty())
                break;
            if (cur.front() == ')') {
                advance(cur, line, col);
                closed = true;
                break;
            }
            Value e = parse_at(S, cur, line, col, name);
            if (e && e.get_type() == TSYMBOL && *e.get_symbol() == ".") {
                skip_ws_and_comments(cur, line, col);
                if (cur.empty())
                    throw LispError(SourceLoc{name, open_line, open_col}, "unexpected EOF after . in list");
                Value tail = parse_at(S, cur, line, col, name);
                *lb.last = std::move(tail);
                skip_ws_and_comments(cur, line, col);
                if (cur.empty() || cur.front() != ')')
                    throw LispError(SourceLoc{name, open_line, open_col}, "expected ) after dotted-tail");
                advance(cur, line, col);
                closed = true;
                break;
            }
            Value *prev_last = lb.last;
            lb.add(S, std::move(e));
            // 在刚创建的 pair 节点上设置源码位置，而非其 cdr（nil）。
            // 这样列表中每个 pair 的 car 都带有位置信息，报错时可从 current_expr 追溯到源码行。
            S.set_source_loc(*prev_last, name, open_line, open_col);
        }
        if (!closed)
            throw LispError(SourceLoc{name, open_line, open_col}, "unexpected EOF while reading list");
        return std::move(lb).done();
    } else if (c == '\'') {
        return parse_quoted(S, cur, line, col, name, "quote");
    } else if (c == '`') {
        return parse_quoted(S, cur, line, col, name, "quasiquote");
    } else if (c == ',') {
        return parse_quoted(S, cur, line, col, name, "unquote");
    } else if (c == '"') {
        size_t sline = line;
        size_t scol = col;

        advance(cur, line, col);
        std::string s;
        while (!cur.empty() && cur.front() != '"') {
            if (cur.front() == '\\' && cur.size() > 1) {
                advance(cur, line, col);
                char esc = cur.front();
                switch (esc) {
                case 'n': s.push_back('\n'); break;
                case 't': s.push_back('\t'); break;
                case 'r': s.push_back('\r'); break;
                case '\\': s.push_back('\\'); break;
                case '"': s.push_back('"'); break;
                default: s.push_back(esc); break;
                }
                advance(cur, line, col);
            } else {
                s.push_back(cur.front());
                advance(cur, line, col);
            }
        }
        if (cur.empty()) {
            throw LispError(SourceLoc{name, sline, scol}, "unexpected EOF while reading string");
        }
        advance(cur, line, col);
        Value v = S.make_string(s);
        S.set_source_loc(v, name, sline, scol);
        return v;
    } else {
        const char *tok_start = cur.data();
        size_t tline = line;
        size_t tcol = col;
        // 批量扫描 token：一次遍历完成行/列追踪，避免逐字节 remove_prefix
        const char *scan = tok_start;
        const char *end_ptr = tok_start + cur.size();
        while (scan < end_ptr && !is_delim(*scan)) {
            if (*scan == '\n') { ++line; col = 1; }
            else { ++col; }
            ++scan;
        }
        cur = std::string_view(scan, end_ptr - scan);
        const char *tok_end = scan;
        double val;
        auto [ptr, ec] = std::from_chars(tok_start, tok_end, val);
        if (ec == std::errc{} && ptr == tok_end) {
            Value v = S.make_number(val);
            S.set_source_loc(v, name, tline, tcol);
            return v;
        }
        std::string_view tok(tok_start, tok_end - tok_start);
        if (tok == "nil")
            return {};
        Value v = S.make_symbol(tok);
        S.set_source_loc(v, name, tline, tcol);
        return v;
    }
}

auto State::parse(const std::string &src, const std::string &name) -> Value {
    sources[name] = src;
    std::string_view cur(src);
    size_t line = 1;
    size_t col = 1;
    return parse_at(*this, cur, line, col, name);
}

auto State::parse_all(const std::string &src, const std::string &name) -> Value {
    sources[name] = src;
    std::string_view cur(src);
    size_t line = 1, col = 1;
    ListBuilder lb;
    while (!cur.empty()) {
        Value e = parse_at(*this, cur, line, col, name);
        lb.add(*this, std::move(e));
    }
    return std::move(lb).done();
}


void State::set_source_loc(const Value &v, std::string_view file, size_t line, size_t col) {
    if (!v)
        return;
    SourceLoc loc;
    loc.file = file;
    loc.line = line;
    loc.col = col;
    src_map[v.identity_key()] = loc;
}

auto State::get_source_loc(const Value &v, SourceLoc &out) const -> bool {
    if (!v)
        return false;
    auto it = src_map.find(v.identity_key());
    if (it == src_map.end())
        return false;
    out = it->second;
    return true;
}

auto State::get_source_line(std::string_view file, size_t line, std::string &out) const -> bool {
    auto it = sources.find(file);
    if (it == sources.end())
        return false;
    const std::string &s = it->second;
    size_t start = 0;
    for (size_t cur = 1; cur < line; ++cur) {
        auto nl = s.find('\n', start);
        if (nl == std::string::npos) return false;
        start = nl + 1;
    }
    if (start >= s.size()) return false;
    auto end = s.find('\n', start);
    out = s.substr(start, end - start);
    return true;
}

void print_error_with_loc(const State &S, const SourceLoc &loc, const std::string &msg) {
    bool color = isatty(fileno(stderr)) || getenv("VDLISP__COLOR");
    auto cerr = [&](const char *s) { if (s) std::cerr << s; };
    auto with_color = [&](const char *c, auto &&print_fn) {
        if (color) std::cerr << c;
        print_fn();
        if (color) std::cerr << "\x1b[0m";
    };
    with_color("\x1b[1;31m", [&]{
        std::cerr << "error: " << loc.file << ":" << loc.line << ":" << loc.col << ": " << msg << "\n";
    });
    std::string line;
    if (S.get_source_line(loc.file, loc.line, line)) {
        with_color("\x1b[1m", [&]{ std::cerr << line << "\n"; });
        size_t col_index = loc.col ? loc.col - 1 : 0;
        std::string caret(col_index, ' ');
        with_color("\x1b[1;31m", [&]{ std::cerr << caret << "^\n"; });
    }
}

void clear_closure_env(Value &v) noexcept {
    // 仅断开闭包对环境的引用指针（不断释放引用计数），
    // 实际 env 释放由 FuncData / MacroData 析构函数统一负责。
    if (!v) return;
    if (v.get_type() == TFUNC) {
        if (auto *fd = v.get_func()) fd->closure_env = nullptr;
    } else if (v.get_type() == TMACRO) {
        if (auto *md = v.get_macro()) md->closure_env = nullptr;
    }
}

auto value_equal(const Value &a, const Value &b) -> bool {
    if (a == b)
        return true;
    if (!a || !b)
        return false;
    if (a.get_type() != b.get_type())
        return false;
    switch (a.get_type()) {
    case TNUMBER:
        return a.get_number() == b.get_number();
    case TSTRING:
        return *a.get_string() == *b.get_string();
    case TSYMBOL:
        return *a.get_symbol() == *b.get_symbol();
    case TPAIR: {
        PairData *ap = a.get_pair();
        PairData *bp = b.get_pair();
        return value_equal(ap->car, bp->car) && value_equal(ap->cdr, bp->cdr);
    }
    default:
        return a == b;
    }
}

} // namespace vdlisp
