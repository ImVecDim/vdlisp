#include "helpers.hpp"
#include <cstdlib>
#include <iostream>
#include <unistd.h>

namespace vdlisp {

// Lisp 读取器里的分隔符集合：一旦遇到这些字符，当前 token 就结束。
static auto is_delim(char c) noexcept -> bool {
    return std::isspace((unsigned char)c) || c == '(' || c == ')' || c == '\'' || c == '"' || c == ';' || c == '`' || c == ',';
}

static auto advance_pos(const std::string &src, size_t &pos, size_t &line, size_t &col) noexcept -> void {
    if (pos >= src.size())
        return;
    char c = src[pos++];
    if (c == '\n') {
        ++line;
        col = 1;
    } else {
        ++col;
    }
}

static auto skip_ws_and_comments(const std::string &src, size_t &pos, size_t &line, size_t &col) noexcept -> void {
    while (pos < src.size()) {
        char c = src[pos];
        if (std::isspace((unsigned char)c)) {
            advance_pos(src, pos, line, col);
            continue;
        }
        if (c == ';') {
            while (pos < src.size() && src[pos] != '\n')
                advance_pos(src, pos, line, col);
            continue;
        }
        break;
    }
}

// quote/quasiquote/unquote 的解析完全相同，只有符号名不同
static auto parse_at(State &S, const std::string &src, size_t &pos, size_t &line, size_t &col, const std::string &name) -> Value;
static auto parse_quoted(State &S, const std::string &src, size_t &pos, size_t &line, size_t &col, const std::string &name, const char *keyword) -> Value {
    size_t qline = line;
    size_t qcol = col;
    advance_pos(src, pos, line, col);
    Value inner = parse_at(S, src, pos, line, col, name);
    Value res = list_of(S, {S.make_symbol(keyword), std::move(inner)});
    S.set_source_loc(res, name, qline, qcol);
    return res;
}


static auto parse_at(State &S, const std::string &src, size_t &pos, size_t &line, size_t &col, const std::string &name) -> Value {
    skip_ws_and_comments(src, pos, line, col);
    if (pos >= src.size()) [[unlikely]]
        return {};
    char c = src[pos];
    if (c == ')') {
        throw LispError(State::SourceLoc{name, line, col}, "unexpected )");
    }
    if (c == '(') {
        size_t open_line = line;
        size_t open_col = col;

        advance_pos(src, pos, line, col);
        ListBuilder lb;
        bool closed = false;
        while (true) {
            skip_ws_and_comments(src, pos, line, col);
            if (pos >= src.size())
                break;
            if (src[pos] == ')') {
                advance_pos(src, pos, line, col);
                closed = true;
                break;
            }
            Value e = parse_at(S, src, pos, line, col, name);
            if (e && e.get_type() == TSYMBOL && *e.get_symbol() == ".") {
                skip_ws_and_comments(src, pos, line, col);
                if (pos >= src.size())
                    throw LispError(State::SourceLoc{name, open_line, open_col}, "unexpected EOF after . in list");
                Value tail = parse_at(S, src, pos, line, col, name);
                *lb.last = std::move(tail);
                skip_ws_and_comments(src, pos, line, col);
                if (pos >= src.size() || src[pos] != ')')
                    throw LispError(State::SourceLoc{name, open_line, open_col}, "expected ) after dotted-tail");
                advance_pos(src, pos, line, col);
                closed = true;
                break;
            }
            lb.add(S, std::move(e));
            S.set_source_loc(*lb.last, name, open_line, open_col);
        }
        if (!closed)
            throw LispError(State::SourceLoc{name, open_line, open_col}, "unexpected EOF while reading list");
        return std::move(lb).done();
    } else if (c == '\'') {
        return parse_quoted(S, src, pos, line, col, name, "quote");
    } else if (c == '`') {
        return parse_quoted(S, src, pos, line, col, name, "quasiquote");
    } else if (c == ',') {
        return parse_quoted(S, src, pos, line, col, name, "unquote");
    } else if (c == '"') {
        size_t sline = line;
        size_t scol = col;

        advance_pos(src, pos, line, col);
        std::string s;
        while (pos < src.size() && src[pos] != '"') {
            if (src[pos] == '\\' && pos + 1 < src.size()) {
                advance_pos(src, pos, line, col);
                char esc = src[pos];
                switch (esc) {
                case 'n':
                    s.push_back('\n');
                    break;
                case 't':
                    s.push_back('\t');
                    break;
                case 'r':
                    s.push_back('\r');
                    break;
                case '\\':
                    s.push_back('\\');
                    break;
                case '"':
                    s.push_back('"');
                    break;
                default:
                    s.push_back(esc);
                    break;
                }
                advance_pos(src, pos, line, col);
            } else {
                s.push_back(src[pos]);
                advance_pos(src, pos, line, col);
            }
        }
        if (pos >= src.size()) {
            throw LispError(State::SourceLoc{name, sline, scol}, "unexpected EOF while reading string");
        }
        // 消费结尾引号后再生成字符串对象。
        advance_pos(src, pos, line, col);
        Value v = S.make_string(s);
        S.set_source_loc(v, name, sline, scol);
        return v;
    } else {
        // 非字符串与列表的 token 要么是 number，要么就是 symbol。
        size_t start = pos;
        size_t tline = line;
        size_t tcol = col;
        while (pos < src.size() && !is_delim(src[pos]))
            advance_pos(src, pos, line, col);
        std::string tok = src.substr(start, pos - start);
        // 先尝试按数字读取；失败再按符号处理。
        char *endp = nullptr;
        double val = strtod(tok.c_str(), &endp);
        if (endp != tok.c_str() && *endp == '\0') {
            Value v = S.make_number(val);
            S.set_source_loc(v, name, tline, tcol);
            return v;
        }
        if (tok == "nil")
            return {};
        Value v = S.make_symbol(tok);
        S.set_source_loc(v, name, tline, tcol);
        return v;
    }
}

auto State::parse(const std::string &src, const std::string &name) -> Value {
    sources[name] = src;
    size_t pos = 0;
    size_t line = 1;
    size_t col = 1;
    return parse_at(*this, src, pos, line, col, name);
}

auto State::parse_all(const std::string &src, const std::string &name) -> Value {
    sources[name] = src;
    size_t pos = 0, line = 1, col = 1;
    ListBuilder lb;
    while (pos < src.size()) {
        Value e = parse_at(*this, src, pos, line, col, name);
        lb.add(*this, std::move(e));
    }
    return std::move(lb).done();
}

auto list_of(State &S, std::initializer_list<Value> items) -> Value {
    ListBuilder lb;
    for (const Value &it : items)
        lb.add(S, Value(it));
    return std::move(lb).done();
}

void State::set_source_loc(const Value &v, const std::string &file, size_t line, size_t col) {
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

auto State::get_source_line(const std::string &file, size_t line, std::string &out) const -> bool {
    auto it = sources.find(file);
    if (it == sources.end())
        return false;
    const std::string &s = it->second;
    size_t cur = 1;
    size_t start = 0;
    size_t i = 0;
    while (cur < line && i < s.size()) {
        if (s[i] == '\n') {
            ++cur;
            ++i;
            start = i;
        } else
            ++i;
    }
    if (start >= s.size())
        return false;
    size_t end = start;
    while (end < s.size() && s[end] != '\n')
        ++end;
    out = s.substr(start, end - start);
    return true;
}

void print_error_with_loc(const State &S, const State::SourceLoc &loc, const std::string &msg) {
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
    if (!v) return;
    Env *env = nullptr;
    auto clear = [&](auto *p) {
        if (p) { env = p->closure_env; p->closure_env = nullptr; }
    };
    if (v.get_type() == TFUNC) clear(v.get_func());
    else if (v.get_type() == TMACRO) clear(v.get_macro());
    if (env) release_env(env);
}

auto value_equal(const Value &a, const Value &b) -> bool {
    // 结构相等用于 Lisp 层的 `=`，pair 采用递归比较。
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

// implementations inlined in header (include/helpers.hpp)

} // namespace vdlisp
