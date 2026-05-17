#ifndef VDLISP__MATH_HPP
#define VDLISP__MATH_HPP

#include "../state.hpp"
#include <cmath>

namespace vdlisp {

// 模板复用一元数值内建的"取参-检查-运算-回包"流程。
template <typename Op>
inline Value builtin_unary_math(State &S, const Value &args, const char *name, Op op) {
    Value arg = require_unary_args(args, name);
    return S.make_number(op(require_number(arg, name)));
}

inline auto register_math(State &S) -> void {
    // 非捕获 lambda 逐行注册（CFunc 是裸函数指针，不能有捕获）
    S.register_builtin("exp",   [](State &S, const Value &a) { return builtin_unary_math(S, a, "exp",   [](double x) { return std::exp(x); }); });
    S.register_builtin("log",   [](State &S, const Value &a) { return builtin_unary_math(S, a, "log",   [](double x) { return std::log(x); }); });
    S.register_builtin("sqrt",  [](State &S, const Value &a) { return builtin_unary_math(S, a, "sqrt",  [](double x) { return std::sqrt(x); }); });
    S.register_builtin("sin",   [](State &S, const Value &a) { return builtin_unary_math(S, a, "sin",   [](double x) { return std::sin(x); }); });
    S.register_builtin("cos",   [](State &S, const Value &a) { return builtin_unary_math(S, a, "cos",   [](double x) { return std::cos(x); }); });
    S.register_builtin("tan",   [](State &S, const Value &a) { return builtin_unary_math(S, a, "tan",   [](double x) { return std::tan(x); }); });
    S.register_builtin("asin",  [](State &S, const Value &a) { return builtin_unary_math(S, a, "asin",  [](double x) { return std::asin(x); }); });
    S.register_builtin("acos",  [](State &S, const Value &a) { return builtin_unary_math(S, a, "acos",  [](double x) { return std::acos(x); }); });
    S.register_builtin("atan",  [](State &S, const Value &a) { return builtin_unary_math(S, a, "atan",  [](double x) { return std::atan(x); }); });
    S.register_builtin("abs",   [](State &S, const Value &a) { return builtin_unary_math(S, a, "abs",   [](double x) { return std::fabs(x); }); });
    S.register_builtin("floor", [](State &S, const Value &a) { return builtin_unary_math(S, a, "floor", [](double x) { return std::floor(x); }); });
    S.register_builtin("ceil",  [](State &S, const Value &a) { return builtin_unary_math(S, a, "ceil",  [](double x) { return std::ceil(x); }); });
    S.register_builtin("round", [](State &S, const Value &a) { return builtin_unary_math(S, a, "round", [](double x) { return std::round(x); }); });
    S.register_builtin("log10", [](State &S, const Value &a) { return builtin_unary_math(S, a, "log10", [](double x) { return std::log10(x); }); });
    S.register_builtin("log2",  [](State &S, const Value &a) { return builtin_unary_math(S, a, "log2",  [](double x) { return std::log2(x); }); });
}

} // namespace vdlisp

#endif // VDLISP__MATH_HPP
