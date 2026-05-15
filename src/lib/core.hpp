#ifndef VDLISP__CORE_HPP
#define VDLISP__CORE_HPP

#include "../helpers.hpp"

#include <cstdlib>
#include <functional>
#include <iostream>
#include <utility>

namespace vdlisp {

// 用模板复用数值内建的"取参-检查-运算-回包"流程。
template <typename Op>
inline Value builtin_arith(State &S, const Value &args, const char *name, Op op) {
    auto [a, b] = require_binary_args(args, name);
    return S.make_number(op(require_number(a, name), require_number(b, name)));
}

template <typename Cmp>
inline Value builtin_cmp(State &S, const Value &args, const char* name, Cmp cmp) {
    auto [a, b] = require_binary_args(args, name);
    return cmp(require_number(a, name), require_number(b, name)) ? S.get_bound("#t", S.global) : Value();
}

inline Value builtin_add(State &S, const Value &args) { return builtin_arith(S, args, "+", std::plus<double>{}); }
inline Value builtin_sub(State &S, const Value &args) { return builtin_arith(S, args, "-", std::minus<double>{}); }
inline Value builtin_mul(State &S, const Value &args) { return builtin_arith(S, args, "*", std::multiplies<double>{}); }
inline Value builtin_div(State &S, const Value &args) {
    return builtin_arith(S, args, "/", [](double a, double b) {
        if (b == 0.0) throw LispError("division by zero");
        return a / b;
    });
}

inline Value builtin_cmp_lt(State &S, const Value &args) { return builtin_cmp(S, args, "<", std::less<double>{}); }
inline Value builtin_cmp_gt(State &S, const Value &args) { return builtin_cmp(S, args, ">", std::greater<double>{}); }
inline Value builtin_cmp_le(State &S, const Value &args) { return builtin_cmp(S, args, "<=", std::less_equal<double>{}); }
inline Value builtin_cmp_ge(State &S, const Value &args) { return builtin_cmp(S, args, ">=", std::greater_equal<double>{}); }

// 注册解释器启动后默认可用的内建函数与特殊形式。
inline auto register_core(State &S) -> void {
    // 基础数值运算与比较全部走统一的参数校验辅助函数。
    S.register_builtin("+", builtin_add);
    S.register_builtin("-", builtin_sub);
    S.register_builtin("*", builtin_mul);
    S.register_builtin("/", builtin_div);
    S.register_builtin("<", builtin_cmp_lt);
    S.register_builtin(">", builtin_cmp_gt);
    S.register_builtin("<=", builtin_cmp_le);
    S.register_builtin(">=", builtin_cmp_ge);

    // 普通内建函数：参数会先求值，再把结果列表传给 C++ 实现。
    S.register_builtin("print", [](State &S, const Value &args) -> Value {
        Value last;
        bool first = true;
        foreach_lisp(args, [&](Value el) {
            std::cout << (std::exchange(first, false) ? "" : " ") << S.to_string(el);
            last = el;
        });
        std::cout << std::endl;
        return last;
    });

    S.register_builtin("list", [](State &, const Value &args) -> Value {
        return args;
    });
    S.register_builtin("type", [](State &S, const Value &args) -> Value {
        Value v = require_unary_args(args, "type");
        return S.make_symbol(v.type_name());
    });
    S.register_builtin("parse", [](State &S, const Value &args) -> Value {
        Value v = require_unary_args(args, "parse");
        if (!v || v.get_type() != TSTRING)
            throw LispError("parse requires a string");
        return S.parse(*v.get_string());
    });
    S.register_builtin("error", [](State &S, const Value &args) -> Value {
        std::string msg = pair_car(args) ? S.to_string(pair_car(args)) : std::string("error");
        throw LispError(msg);
    });

    S.register_builtin("cons", [](State &S, const Value &args) -> Value {
        auto [a, b] = require_binary_args(args, "cons");
        return S.make_pair(std::move(a), std::move(b));
    });
    S.register_builtin("car", [](State &, const Value &args) -> Value {
        Value v = require_pair_arg(args, "car");
        if (!v)
            return {};
        return pair_car(v);
    });
    S.register_builtin("cdr", [](State &, const Value &args) -> Value {
        Value v = require_pair_arg(args, "cdr");
        if (!v)
            return {};
        return pair_cdr(v);
    });
    S.register_builtin("setcar", [](State &, const Value &args) -> Value {
        auto [p, v] = require_binary_args(args, "setcar");
        if (!p || p.get_type() != TPAIR) throw LispError("setcar expects a pair");
        pair_set_car(p, v); return v;
    });
    S.register_builtin("setcdr", [](State &, const Value &args) -> Value {
        auto [p, v] = require_binary_args(args, "setcdr");
        if (!p || p.get_type() != TPAIR) throw LispError("setcdr expects a pair");
        pair_set_cdr(p, v); return v;
    });

    S.register_builtin("=", [](State &S, const Value &args) -> Value {
        auto [a, b] = require_binary_args(args, "=");
        return value_equal(a, b) ? S.get_bound("#t", S.global) : Value();
    });

    S.register_builtin("exit", [](State &S, const Value &args) -> Value {
        int code = 0;
        if (args)
            code = (int)require_number(require_unary_args(args, "exit"), "exit");
        // Ensure pooled memory is released before terminating the process.
        S.shutdown_and_purge_pools();
        std::exit(code);
        return {};
    });

    // 特殊形式：接收未经求值的参数列表，并自行决定求值策略。
    S.register_prim("quote", [](State &, const Value &args, Env *) -> Value {
        return pair_car(args);
    });
    S.register_prim("unquote", [](State &S, const Value &args, Env *env) -> Value {
        return pair_car(args) ? S.eval(pair_car(args), env) : Value();
    });
    S.register_prim("quasiquote", [](State &S, const Value &args, Env *env) -> Value {
        // 递归展开 quasiquote；只有最内层的 unquote 会真正触发求值。
        std::function<Value(const Value &, int)> qq_expand = [&](const Value &expr, int depth) -> Value {
            if (!expr)
                return {};
            if (is_pair(expr)) {
                Value car = pair_car(expr);
                Value cdr = pair_cdr(expr);
                if (is_symbol(car, "unquote")) {
                    if (depth == 1) {
                        Value uq_args = cdr;
                        return uq_args ? S.eval(pair_car(uq_args), env) : Value();
                    } else {
                        return S.make_pair(std::move(car), qq_expand(cdr, depth - 1));
                    }
                }
                if (is_symbol(car, "quasiquote")) {
                    return S.make_pair(std::move(car), qq_expand(cdr, depth + 1));
                }
                return S.make_pair(qq_expand(car, depth), qq_expand(cdr, depth));
            }
            return expr;
        };
        return qq_expand(pair_car(args), 1);
    });
    // `if` 不作为 primitive 存在，而是由语言层宏基于 `cond` 提供。
    S.register_prim("set", [](State &S, const Value &args, Env *env) -> Value {
        Value sym = pair_car(args);
        Value valexpr = pair_car(pair_cdr(args));
        Value val = S.eval(valexpr, env);
        return S.set(sym, std::move(val), env);
    });
    S.register_prim("fn", [](State &S, const Value &args, Env *env) -> Value {
        Value params = pair_car(args);
        Value body = pair_cdr(args);
        return S.make_function(std::move(params), std::move(body), env);
    });
    S.register_prim("macro", [](State &S, const Value &args, Env *env) -> Value {
        Value params = pair_car(args);
        Value body = pair_cdr(args);
        return S.make_macro(std::move(params), std::move(body), env);
    });
    S.register_prim("let", [](State &S, const Value &args, Env *env) -> Value {
        Value vars = pair_car(args);
        Env *e = S.make_env(env);
        EnvGuard eg(e);
        // `let` 绑定按顺序建立，后续绑定可以看到前面的结果。
        while (vars) {
            Value sym = pair_car(vars);
            vars = pair_cdr(vars);
            Value val = pair_car(vars);
            val = S.eval(val, e);
            (void)S.bind(sym, std::move(val), e);
            vars = pair_cdr(vars);
        }
        return S.do_list(pair_cdr(args), e);
    });
    S.register_prim("while", [](State &S, const Value &args, Env *env) -> Value {
        Value cond = pair_car(args);
        Value body = pair_cdr(args);
        Value res;
        while (S.eval(cond, env)) {
            res = S.do_list(body, env);
        }
        return res;
    });
    // `cond` 直接在解释器里实现，避免再依赖语言层宏，便于作为其它控制结构的基础。
    S.register_prim("cond", [](State &S, const Value &args, Env *env) -> Value {
        Value clauses = args;
        while (clauses) {
            Value clause = pair_car(clauses);
            if (!clause) {
                clauses = pair_cdr(clauses);
                continue;
            }
            Value test = pair_car(clause);
            Value body = pair_cdr(clause);
            Value tval = S.eval(test, env);
            if (tval)
                return S.do_list(body, env);
            clauses = pair_cdr(clauses);
        }
        return S.make_nil();
    });

    // `apply` 负责把"函数 + 实参列表"重新交给统一调用入口。
    S.register_prim("apply", [](State &S, const Value &args, Env *env) -> Value {
        Value fnexpr = pair_car(args);
        if (!fnexpr)
            throw LispError("apply requires a function");
        Value listexpr = pair_car(pair_cdr(args));
        Value fn = S.eval(fnexpr, env);
        Value list = S.eval(listexpr, env);
        return S.call(fn, list, env);
    });
}

} // namespace vdlisp

#endif // VDLISP__CORE_HPP
