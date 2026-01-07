#include "core.hpp"
#include "helpers.hpp"
#include "require.hpp"
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <optional>
#include <sstream>

using namespace vdlisp;

namespace vdlisp {

template <typename Op>
static auto arith_binary(
    State &S,
    const Value &args,
    Op op,
    const char *name) -> Value {
    if (!args || !pair_cdr(args) || pair_cdr(pair_cdr(args)))
        throw std::runtime_error(std::string(name) + " requires exactly two arguments");
    double a = require_number(pair_car(args), name);
    double b = require_number(pair_car(pair_cdr(args)), name);
    return S.make_number(op(a, b));
}

template <typename Cmp>
static auto compare_binary(
    State &S,
    const Value &args,
    Cmp cmp,
    const char *name) -> Value {
    if (!args || !pair_cdr(args) || pair_cdr(pair_cdr(args)))
        throw std::runtime_error(std::string(name) + " requires exactly two arguments");
    double a = require_number(pair_car(args), name);
    double b = require_number(pair_car(pair_cdr(args)), name);
    return cmp(a, b) ? S.get_bound("#t", S.global) : Value();
}

// arithmetic builtins (file-scope wrappers)
static Value builtin_add(State &S, const Value &args) { return arith_binary(S, args, std::plus<double>{}, "+"); }
static Value builtin_sub(State &S, const Value &args) { return arith_binary(S, args, std::minus<double>{}, "-"); }
static Value builtin_mul(State &S, const Value &args) { return arith_binary(S, args, std::multiplies<double>{}, "*"); }
static Value builtin_div(State &S, const Value &args) {
    return arith_binary(S, args, [](double a, double b) -> double { if (b == 0.0) throw std::runtime_error("division by zero"); return a / b; }, "/");
}

// comparison builtins (file-scope wrappers)
static Value builtin_cmp_lt(State &S, const Value &args) { return compare_binary(S, args, std::less<double>{}, "<"); }
static Value builtin_cmp_gt(State &S, const Value &args) { return compare_binary(S, args, std::greater<double>{}, ">"); }
static Value builtin_cmp_le(State &S, const Value &args) { return compare_binary(S, args, std::less_equal<double>{}, "<="); }
static Value builtin_cmp_ge(State &S, const Value &args) { return compare_binary(S, args, std::greater_equal<double>{}, ">="); }

void register_core(State &S) {
    // --- builtins ---
    S.register_builtin("print", [](State &S, const Value &args) -> Value {
        Value last = Value();
        bool first = true;
        Value cur = args;
        while (cur) {
            if (!first)
                std::cout << ' ';
            Value el = pair_car(cur);
            std::cout << S.to_string(el);
            first = false;
            last = el;
            cur = pair_cdr(cur);
        }
        std::cout << '\n';
        return last;
    });

    struct {
        const char *n;
        Value (*f)(State &, const Value &);
    } ops[] = {
        {"+", builtin_add}, {"-", builtin_sub}, //
        {"*", builtin_mul}, {"/", builtin_div}, //
        {"<", builtin_cmp_lt}, {">", builtin_cmp_gt}, //
        {"<=", builtin_cmp_le}, {">=", builtin_cmp_ge}};
    for (auto &op : ops)
        S.register_builtin(op.n, op.f);
    S.register_builtin("list", [](State &, const Value &args) -> Value {
        return args;
    });
    S.register_builtin("type", [](State &S, const Value &args) -> Value {
        Value v = pair_car(args);
        return S.make_symbol(type_name(v));
    });
    S.register_builtin("parse", [](State &S, const Value &args) -> Value {
        if (!args || !pair_car(args) || pair_car(args).get_type() != TSTRING)
            throw std::runtime_error("parse requires a string");
        return S.parse(*pair_car(args).get_string());
    });
    S.register_builtin("error", [](State &S, const Value &args) -> Value {
        std::string msg = pair_car(args) ? S.to_string(pair_car(args)) : std::string("error");
        throw std::runtime_error(msg);
    });

    S.register_builtin("cons", [](State &S, const Value &args) -> Value {
        Value a = pair_car(args);
        Value b = pair_car(pair_cdr(args));
        return S.make_pair(std::move(a), std::move(b));
    });
    S.register_builtin("car", [](State &, const Value &args) -> Value {
        Value v = pair_car(args);
        if (!v)
            return {};
        if (v.get_type() != TPAIR)
            throw std::runtime_error("car expects a pair");
        return pair_car(v);
    });
    S.register_builtin("cdr", [](State &, const Value &args) -> Value {
        Value v = pair_car(args);
        if (!v)
            return {};
        if (v.get_type() != TPAIR)
            throw std::runtime_error("cdr expects a pair");
        return pair_cdr(v);
    });
    S.register_builtin("setcar", [](State &, const Value &args) -> Value {
        Value p = pair_car(args);
        Value v = pair_car(pair_cdr(args));
        if (!p || p.get_type() != TPAIR)
            throw std::runtime_error("setcar expects a pair");
        pair_set_car(p, v);
        return v;
    });
    S.register_builtin("setcdr", [](State &, const Value &args) -> Value {
        Value p = pair_car(args);
        Value v = pair_car(pair_cdr(args));
        if (!p || p.get_type() != TPAIR)
            throw std::runtime_error("setcdr expects a pair");
        pair_set_cdr(p, v);
        return v;
    });

    S.register_builtin("=", [](State &S, const Value &args) -> Value {
        if (!args || !pair_cdr(args) || pair_cdr(pair_cdr(args)))
            throw std::runtime_error("= requires exactly two arguments");
        Value a = pair_car(args);
        Value b = pair_car(pair_cdr(args));
        return value_equal(a, b) ? S.get_bound("#t", S.global) : Value();
    });

    S.register_builtin("exit", [](State &S, const Value &args) -> Value {
        int code = 0;
        if (pair_car(args))
            code = (int)require_number(pair_car(args), "exit");
        // Ensure pooled memory is released before terminating the process.
        S.shutdown_and_purge_pools();
        std::exit(code);
        return {};
    });

    // use centralized require implementation
    register_require(S);

    // --- prims ---
    S.register_prim("quote", [](State &, const Value &args, Env *) -> Value {
        return pair_car(args);
    });
    S.register_prim("unquote", [](State &S, const Value &args, Env *env) -> Value {
        return pair_car(args) ? S.eval(pair_car(args), env) : Value();
    });
    S.register_prim("quasiquote", [](State &S, const Value &args, Env *env) -> Value {
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
    // `if` removed as a primitive; provide it via a macro implemented using `cond`.
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
    // cond special form: evaluate clauses sequentially; for the first true
    // test evaluate and return the body. Implemented directly to avoid
    // depending on `if` (which may be provided at the language level as a macro).
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

    // Provide `if` as a language-level macro implemented via `cond`.
    // This creates a proper TMACRO binding in the global environment so user
    // scripts can rely on `if` even though it's not a primitive.
    S.register_prim("apply", [](State &S, const Value &args, Env *env) -> Value {
        Value fnexpr = pair_car(args);
        if (!fnexpr)
            throw std::runtime_error("apply requires a function");
        Value listexpr = pair_car(pair_cdr(args));
        Value fn = S.eval(fnexpr, env);
        Value list = S.eval(listexpr, env);
        return S.call(fn, list, env);
    });
}

} // namespace vdlisp