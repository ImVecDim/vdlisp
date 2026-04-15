#ifndef VDLISP__HELPERS_HPP
#define VDLISP__HELPERS_HPP

#include "vdlisp.hpp"

#include <stdexcept>
#include <utility>

namespace vdlisp {

// 解释器统一异常类型：既能携带文本，也能附带源码位置与调用链。
struct LispError : public std::runtime_error {
    State::SourceLoc loc;
    std::vector<State::SourceLoc> call_chain;
    bool has_loc = false;

    LispError(const std::string &msg)
        : std::runtime_error(msg), has_loc(false) {}
    LispError(State::SourceLoc loc, const std::string &msg)
        : std::runtime_error(msg), loc(std::move(loc)), has_loc(true) {}
    LispError(State::SourceLoc loc, const std::string &msg, std::vector<State::SourceLoc> chain)
        : std::runtime_error(msg), loc(std::move(loc)), call_chain(std::move(chain)), has_loc(true) {}
};

// 打印带源码定位的错误信息，供 REPL 和批处理入口复用。
auto print_error_with_loc(const State &S, const State::SourceLoc &loc, const std::string &msg) -> void;

[[nodiscard]] auto value_equal(const Value &a, const Value &b) -> bool;

// 这类小函数处于热路径，直接内联可减少解释器分派开销。
[[nodiscard]] inline __attribute__((always_inline)) auto type_name(const Value &v) -> std::string {
    if (!v)
        return std::string("nil");
    return v.type_name();
}

[[nodiscard]] inline __attribute__((always_inline)) auto require_number(const Value &v, const char *who) -> double {
    if (!v || v.get_type() != TNUMBER) [[unlikely]]
        throw LispError(std::string(who) + std::string(": expected number, got ") + std::string(type_name(v)));
    return v.get_number();
}

// 对 pair 的访问在整个解释器里极其频繁，统一封装可让主逻辑更可读。
[[nodiscard]] inline __attribute__((always_inline)) auto pair_car(const Value &p) noexcept -> Value {
    if (!p)
        return {};
    if (p.get_type() != TPAIR)
        return {};
    return p.get_pair()->car;
}
[[nodiscard]] inline __attribute__((always_inline)) auto pair_cdr(const Value &p) noexcept -> Value {
    if (!p)
        return {};
    if (p.get_type() != TPAIR)
        return {};
    return p.get_pair()->cdr;
}
[[nodiscard]] inline __attribute__((always_inline)) auto is_pair(const Value &p) noexcept -> bool {
    return p && p.get_type() == TPAIR;
}
[[nodiscard]] inline __attribute__((always_inline)) auto is_symbol(const Value &p, const std::string &name) -> bool {
    return p && p.get_type() == TSYMBOL && *p.get_symbol() == name;
}
inline __attribute__((always_inline)) void pair_set_car(const Value &p, const Value &v) noexcept {
    if (!p)
        return;
    if (p.get_type() != TPAIR)
        return;
    p.get_pair()->car = v;
}
inline __attribute__((always_inline)) void pair_set_cdr(const Value &p, const Value &v) noexcept {
    if (!p)
        return;
    if (p.get_type() != TPAIR)
        return;
    p.get_pair()->cdr = v;
}

// 断开闭包对环境的引用，主要用于进程退出前主动打破引用环。
void clear_closure_env(Value &v) noexcept;

inline void foreach_lisp(Value list, auto&& F) {
    for (Value o = list; o; o = pair_cdr(o)) F(pair_car(o));
}

} // namespace vdlisp

#endif