#include "helpers.hpp"
#include <cctype>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <unistd.h>

namespace vdlisp {

static auto is_delim(char c) noexcept -> bool {
    return std::isspace((unsigned char)c) || c == '(' || c == ')' || c == '\'' || c == '"' || c == ';' || c == '`' || c == ',';
}

static void advance_pos(const std::string &src, size_t &pos, size_t &line, size_t &col) noexcept {
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

static void skip_ws_and_comments(const std::string &src, size_t &pos, size_t &line, size_t &col) noexcept {
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

// parser implementation; kept in src/helpers.cpp via non-member parse_at

static auto parse_at(State &S, const std::string &src, size_t &pos, size_t &line, size_t &col, const std::string &name) -> Value {
    skip_ws_and_comments(src, pos, line, col);
    if (pos >= src.size()) [[unlikely]]
        return {};
    char c = src[pos];
    if (c == ')') {
        throw ParseError(State::SourceLoc{name, line, col}, "unexpected )");
    }
    if (c == '(') {
        size_t open_line = line;
        size_t open_col = col;

        advance_pos(src, pos, line, col);
        Value head = nullptr;
        Value *last = &head;
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
            // Parse next element. If it's the dot symbol "." then treat the
            // following expression as the dotted-tail (cdr) of the list.
            Value e = parse_at(S, src, pos, line, col, name);
            if (e && e.get_type() == TSYMBOL && *e.get_symbol() == ".") {
                // dotted-tail: parse the tail expression and splice it as the cdr
                skip_ws_and_comments(src, pos, line, col);
                if (pos >= src.size())
                    throw ParseError(State::SourceLoc{name, open_line, open_col}, "unexpected EOF after . in list");
                Value tail = parse_at(S, src, pos, line, col, name);
                // set the cdr pointer of the last pair (pointed to by `last`) to tail
                *last = tail;
                // after a dotted-tail the list must be closed immediately
                skip_ws_and_comments(src, pos, line, col);
                if (pos >= src.size() || src[pos] != ')')
                    throw ParseError(State::SourceLoc{name, open_line, open_col}, "expected ) after dotted-tail");
                advance_pos(src, pos, line, col);
                closed = true;
                break;
            }
            // Otherwise append the parsed element to the list as before.
            *last = S.make_pair(std::move(e), Value());
            PairData *pd = (*last).get_pair();
            S.set_source_loc(*last, name, open_line, open_col);
            last = &pd->cdr;
        }
        if (!closed) {
            throw ParseError(State::SourceLoc{name, open_line, open_col}, "unexpected EOF while reading list");
        }
        return head;
    } else if (c == '\'') {
        size_t qline = line;
        size_t qcol = col;

        advance_pos(src, pos, line, col);
        Value quoted = parse_at(S, src, pos, line, col, name);
        Value res = list_of(S, {S.make_symbol("quote"), quoted});
        S.set_source_loc(res, name, qline, qcol);
        return res;
    } else if (c == '`') {
        size_t qline = line;
        size_t qcol = col;

        advance_pos(src, pos, line, col);
        Value qq = parse_at(S, src, pos, line, col, name);
        Value res = list_of(S, {S.make_symbol("quasiquote"), qq});
        S.set_source_loc(res, name, qline, qcol);
        return res;
    } else if (c == ',') {
        size_t qline = line;
        size_t qcol = col;

        advance_pos(src, pos, line, col);
        Value uq = parse_at(S, src, pos, line, col, name);
        Value res = list_of(S, {S.make_symbol("unquote"), uq});
        S.set_source_loc(res, name, qline, qcol);
        return res;
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
            throw ParseError(State::SourceLoc{name, sline, scol}, "unexpected EOF while reading string");
        }
        // consume closing quote
        advance_pos(src, pos, line, col);
        Value v = S.make_string(s);
        S.set_source_loc(v, name, sline, scol);
        return v;
    } else {
        // symbol or number
        size_t start = pos;
        size_t tline = line;
        size_t tcol = col;
        while (pos < src.size() && !is_delim(src[pos]))
            advance_pos(src, pos, line, col);
        std::string tok = src.substr(start, pos - start);
        // try number
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
    size_t pos = 0;
    size_t line = 1;
    size_t col = 1;
    Value head;
    Value *last = &head;
    while (pos < src.size()) {
        Value e = parse_at(*this, src, pos, line, col, name);
        *last = make_pair(std::move(e), Value());
        PairData *pd = (*last).get_pair();
        last = &pd->cdr;
    }
    return head;
}

auto list_of(State &S, std::initializer_list<Value> items) -> Value {
    Value head;
    Value *last = &head;
    for (auto &it : items) {
        // `it` is from initializer_list; copy as before (moving here would
        // not be safe for callers). Keep unchanged.
        *last = S.make_pair(it, Value());
        PairData *pd = (*last).get_pair();
        last = &pd->cdr;
    }
    return head;
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
    const char *c_red = "\x1b[1;31m";
    const char *c_bold = "\x1b[1m";
    const char *c_reset = "\x1b[0m";

    if (color)
        std::cerr << c_red;
    std::cerr << "error: " << loc.file << ":" << loc.line << ":" << loc.col << ": " << msg << "\n";
    if (color)
        std::cerr << c_reset;

    std::string line;
    if (S.get_source_line(loc.file, loc.line, line)) {
        if (color)
            std::cerr << c_bold << line << c_reset << "\n";
        else
            std::cerr << line << "\n";

        size_t col_index = loc.col ? loc.col - 1 : 0;
        std::string caret_spaces;
        for (size_t i = 0; i < col_index; ++i)
            caret_spaces.push_back((i < line.size() && line[i] == '\t') ? '\t' : ' ');

        if (color)
            std::cerr << caret_spaces << c_red << "^" << c_reset << "\n";
        else
            std::cerr << caret_spaces << "^" << "\n";
    }
}

// helper: clear closure_env held by TFUNC/TMACRO Values
void clear_closure_env(Value &v) noexcept {
    if (!v)
        return;
    if (v.get_type() == TFUNC) {
        FuncData *fd = v.get_func();
        if (fd && fd->closure_env) {
            release_env(fd->closure_env);
            fd->closure_env = nullptr;
        }
    } else if (v.get_type() == TMACRO) {
        MacroData *md = v.get_macro();
        if (md && md->closure_env) {
            release_env(md->closure_env);
            md->closure_env = nullptr;
        }
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

// implementations inlined in header (include/helpers.hpp)

} // namespace vdlisp
