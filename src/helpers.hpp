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
// 用模板消除 pair_car/pair_cdr 中的重复。
template <auto Member>
[[nodiscard]] inline __attribute__((always_inline)) auto pair_access(const Value &p) noexcept -> Value {
    if (!p || p.get_type() != TPAIR)
        return {};
    return (p.get_pair()->*Member);
}
[[nodiscard]] inline __attribute__((always_inline)) auto pair_car(const Value &p) noexcept -> Value { return pair_access<&PairData::car>(p); }
[[nodiscard]] inline __attribute__((always_inline)) auto pair_cdr(const Value &p) noexcept -> Value { return pair_access<&PairData::cdr>(p); }
[[nodiscard]] inline __attribute__((always_inline)) auto is_pair(const Value &p) noexcept -> bool {
    return p && p.get_type() == TPAIR;
}
[[nodiscard]] inline __attribute__((always_inline)) auto is_symbol(const Value &p, const std::string &name) -> bool {
    return p && p.get_type() == TSYMBOL && *p.get_symbol() == name;
}
// 用模板消除 pair_set_car/pair_set_cdr 中的重复。
template <auto Member>
inline __attribute__((always_inline)) void pair_set(const Value &p, const Value &v) noexcept {
    if (!p || p.get_type() != TPAIR)
        return;
    p.get_pair()->*Member = v;
}
inline __attribute__((always_inline)) void pair_set_car(const Value &p, const Value &v) noexcept { pair_set<&PairData::car>(p, v); }
inline __attribute__((always_inline)) void pair_set_cdr(const Value &p, const Value &v) noexcept { pair_set<&PairData::cdr>(p, v); }

// 断开闭包对环境的引用，主要用于进程退出前主动打破引用环。
void clear_closure_env(Value &v) noexcept;

inline void foreach_lisp(Value list, auto&& F) {
    for (Value o = list; o; o = pair_cdr(o)) F(pair_car(o));
}

// 统一检查一元内建参数，避免每个 builtin 自己重复拆表。
[[nodiscard]] inline __attribute__((always_inline)) auto require_unary_args(const Value &args, const char *name) -> Value {
    if (!args || pair_cdr(args))
        throw LispError(std::string(name) + " requires exactly one argument");
    return pair_car(args);
}

[[nodiscard]] inline __attribute__((always_inline)) auto require_binary_args(const Value &args, const char *name) -> std::pair<Value, Value> {
    if (!args || !pair_cdr(args) || pair_cdr(pair_cdr(args)))
        throw LispError(std::string(name) + " requires exactly two arguments");
    return {pair_car(args), pair_car(pair_cdr(args))};
}

[[nodiscard]] inline __attribute__((always_inline)) auto require_pair_arg(const Value &args, const char *name) -> Value {
    Value v = require_unary_args(args, name);
    if (v && v.get_type() != TPAIR)
        throw LispError(std::string(name) + " expects a pair");
    return v;
}

} // namespace vdlisp

#endif