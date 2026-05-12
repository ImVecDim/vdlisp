#ifndef VDLISP__MATH_HPP
#define VDLISP__MATH_HPP

#include "../helpers.hpp"
#include "../nanbox.hpp"
#include <cmath>

namespace vdlisp {

// 模板复用一元数值内建的"取参-检查-运算-回包"流程。
template <typename Op>
inline Value builtin_unary_math(State &S, const Value &args, const char *name, Op op) {
    Value arg = require_unary_args(args, name);
    return S.make_number(op(require_number(arg, name)));
}

inline auto register_math(State &S) -> void {
    // 一元数学函数：参数会先求值，再把结果传给 C++ 实现。
    S.register_builtin("exp",   [](State &S, const Value &args) -> Value {
        return builtin_unary_math(S, args, "exp",   [](double x) { return std::exp(x); });
    });
    S.register_builtin("log",   [](State &S, const Value &args) -> Value {
        return builtin_unary_math(S, args, "log",   [](double x) { return std::log(x); });
    });
    S.register_builtin("sqrt",  [](State &S, const Value &args) -> Value {
        return builtin_unary_math(S, args, "sqrt",  [](double x) { return std::sqrt(x); });
    });
    S.register_builtin("sin",   [](State &S, const Value &args) -> Value {
        return builtin_unary_math(S, args, "sin",   [](double x) { return std::sin(x); });
    });
    S.register_builtin("cos",   [](State &S, const Value &args) -> Value {
        return builtin_unary_math(S, args, "cos",   [](double x) { return std::cos(x); });
    });
    S.register_builtin("tan",   [](State &S, const Value &args) -> Value {
        return builtin_unary_math(S, args, "tan",   [](double x) { return std::tan(x); });
    });
    S.register_builtin("asin",  [](State &S, const Value &args) -> Value {
        return builtin_unary_math(S, args, "asin",  [](double x) { return std::asin(x); });
    });
    S.register_builtin("acos",  [](State &S, const Value &args) -> Value {
        return builtin_unary_math(S, args, "acos",  [](double x) { return std::acos(x); });
    });
    S.register_builtin("atan",  [](State &S, const Value &args) -> Value {
        return builtin_unary_math(S, args, "atan",  [](double x) { return std::atan(x); });
    });
    S.register_builtin("abs",   [](State &S, const Value &args) -> Value {
        return builtin_unary_math(S, args, "abs",   [](double x) { return std::fabs(x); });
    });
    S.register_builtin("floor", [](State &S, const Value &args) -> Value {
        return builtin_unary_math(S, args, "floor", [](double x) { return std::floor(x); });
    });
    S.register_builtin("ceil",  [](State &S, const Value &args) -> Value {
        return builtin_unary_math(S, args, "ceil",  [](double x) { return std::ceil(x); });
    });
    S.register_builtin("round", [](State &S, const Value &args) -> Value {
        return builtin_unary_math(S, args, "round", [](double x) { return std::round(x); });
    });
    S.register_builtin("log10", [](State &S, const Value &args) -> Value {
        return builtin_unary_math(S, args, "log10", [](double x) { return std::log10(x); });
    });
    S.register_builtin("log2",  [](State &S, const Value &args) -> Value {
        return builtin_unary_math(S, args, "log2",  [](double x) { return std::log2(x); });
    });
}

} // namespace vdlisp

#endif // VDLISP__MATH_HPP
