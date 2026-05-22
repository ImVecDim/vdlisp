#ifndef VDLISP__BUILTIN_HELPERS_HPP
#define VDLISP__BUILTIN_HELPERS_HPP

// ============================================================================
// 内建函数参数校验工具：仅被 src/lib/ 内的 core/math 等使用。
// ============================================================================

#include "vdlisp.hpp"

namespace vdlisp {

// 数值参数检查
[[nodiscard]] [[gnu::always_inline]] inline auto
require_number(const Value &v, const char *who) -> double {
    if (!v || v.get_type() != TNUMBER) [[unlikely]]
        throw LispError(std::string(who) + ": expected number, got "
                        + std::string(v.type_name()));
    return v.get_number();
}

// 单参数校验
[[nodiscard]] [[gnu::always_inline]] inline auto
require_unary_args(const Value &args, const char *name) -> Value {
    if (!args || pair_cdr(args))
        throw LispError(std::string(name) + " requires exactly one argument");
    return pair_car(args);
}

// 双参数校验
[[nodiscard]] [[gnu::always_inline]] inline auto
require_binary_args(const Value &args, const char *name)
    -> std::pair<Value, Value> {
    if (!args || !pair_cdr(args) || pair_cdr(pair_cdr(args)))
        throw LispError(std::string(name) + " requires exactly two arguments");
    return {pair_car(args), pair_car(pair_cdr(args))};
}

// pair 类型参数校验（先取单参，再查 pair 类型）
[[nodiscard]] [[gnu::always_inline]] inline auto
require_pair_arg(const Value &args, const char *name) -> Value {
    Value v = require_unary_args(args, name);
    if (v && v.get_type() != TPAIR)
        throw LispError(std::string(name) + " expects a pair");
    return v;
}

} // namespace vdlisp

#endif // VDLISP__BUILTIN_HELPERS_HPP
