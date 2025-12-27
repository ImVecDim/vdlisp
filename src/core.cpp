#include "core.hpp"
#include "helpers.hpp"
#include <functional>
#include <iostream>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <optional>
#include "require.hpp"

using namespace vdlisp;

namespace vdlisp
{

static auto arith_binary(
  State &S,
  Ptr args,
  double (*op)(double, double),
  const char *name) -> Ptr
{
  if (!args || !pair_cdr(args) || pair_cdr(pair_cdr(args)))
    throw std::runtime_error(std::string(name) + " requires exactly two arguments");
  double a = require_number(pair_car(args), name);
  double b = require_number(pair_car(pair_cdr(args)), name);
  return S.make_number(op(a, b));
}

static auto compare_chain(
  State &S,
  Ptr args,
  bool (*cmp)(double, double),
  const char *name) -> Ptr
{
  if (!args)
    throw std::runtime_error(std::string(name) + " needs arguments");
  double a = require_number(pair_car(args), name);
  Ptr cur = pair_cdr(args);
  while (cur)
  {
    double b = require_number(pair_car(cur), name);
    if (!cmp(a, b))
      return {};
    a = b;
    cur = pair_cdr(cur);
  }
  return S.get_bound("t", S.global);
}

void register_core(State &S)
{
  // --- builtins ---
  S.register_builtin("print", [](State &S, Ptr args) -> Ptr {
    Ptr last = Ptr();
    bool first = true;
    while (args)
    {
      if (!first)
        std::cout << ' ';
      Ptr el = pair_car(args);
      std::cout << S.to_string(el);
      first = false;
      last = el;
      args = pair_cdr(args);
    }
    std::cout << '\n';
    return last;
  });

  S.register_builtin("+", [](State &S, Ptr args) -> Ptr {
    return arith_binary(S, args,
             [](double a, double b) -> double { return a + b; },
             "+");
  });
  S.register_builtin("-", [](State &S, Ptr args) -> Ptr {
    return arith_binary(S, args,
             [](double a, double b) -> double { return a - b; },
             "-");
  });
  S.register_builtin("*", [](State &S, Ptr args) -> Ptr {
    return arith_binary(S, args,
             [](double a, double b) -> double { return a * b; },
             "*");
  });
  S.register_builtin("/", [](State &S, Ptr args) -> Ptr {
    return arith_binary(S, args,
             [](double a, double b) -> double {
               if (b == 0.0)
                 throw std::runtime_error("division by zero");
               return a / b;
             },
             "/");
  });

  S.register_builtin("<", [](State &S, Ptr args) -> Ptr {
    return compare_chain(S, args,
              [](double a, double b) -> bool { return a < b; },
              "<");
  });
  S.register_builtin(">", [](State &S, Ptr args) -> Ptr {
    return compare_chain(S, args,
              [](double a, double b) -> bool { return a > b; },
              ">");
  });
  S.register_builtin("<=", [](State &S, Ptr args) -> Ptr {
    return compare_chain(S, args,
              [](double a, double b) -> bool { return a <= b; },
              "<=");
  });
  S.register_builtin(">=", [](State &S, Ptr args) -> Ptr {
    return compare_chain(S, args,
              [](double a, double b) -> bool { return a >= b; },
              ">=");
  });
  S.register_builtin("list", [](State &, Ptr args) -> Ptr {
    return args;
  });
  S.register_builtin("type", [](State &S, Ptr args) -> Ptr {
    Ptr v = pair_car(args);
    return S.make_symbol(type_name(v));
  });
  S.register_builtin("parse", [](State &S, Ptr args) -> Ptr {
    if (!args || !pair_car(args) || pair_car(args)->get_type() != TSTRING)
      throw std::runtime_error("parse requires a string");
    return S.parse(*pair_car(args)->get_string());
  });
  S.register_builtin("error", [](State &S, Ptr args) -> Ptr {
    std::string msg = pair_car(args) ? S.to_string(pair_car(args)) : std::string("error");
    throw std::runtime_error(msg);
    return {};
  });

  S.register_builtin("cons", [](State &S, Ptr args) -> Ptr {
    Ptr a = pair_car(args);
    Ptr b = pair_car(pair_cdr(args));
    return S.make_pair(a, b);
  });
  S.register_builtin("car", [](State &, Ptr args) -> Ptr {
    Ptr v = pair_car(args);
    if (!v)
      return {};
    if (v->get_type() != TPAIR)
      throw std::runtime_error("car expects a pair");
    return pair_car(v);
  });
  S.register_builtin("cdr", [](State &, Ptr args) -> Ptr {
    Ptr v = pair_car(args);
    if (!v)
      return {};
    if (v->get_type() != TPAIR)
      throw std::runtime_error("cdr expects a pair");
    return pair_cdr(v);
  });
  S.register_builtin("setcar", [](State &, Ptr args) -> Ptr {
    Ptr p = pair_car(args);
    Ptr v = pair_car(pair_cdr(args));
    if (!p || p->get_type() != TPAIR)
      throw std::runtime_error("setcar expects a pair");
    pair_set_car(p, v);
    return v;
  });
  S.register_builtin("setcdr", [](State &, Ptr args) -> Ptr {
    Ptr p = pair_car(args);
    Ptr v = pair_car(pair_cdr(args));
    if (!p || p->get_type() != TPAIR)
      throw std::runtime_error("setcdr expects a pair");
    pair_set_cdr(p, v);
    return v;
  });

  S.register_builtin("=", [](State &S, Ptr args) -> Ptr {
    Ptr a = pair_car(args);
    Ptr b = pair_car(pair_cdr(args));
    return value_equal(a, b) ? S.get_bound("t", S.global) : Ptr();
  });

  S.register_builtin("exit", [](State &S, Ptr args) -> Ptr {
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
  S.register_prim("quote", [](State &, Ptr args, std::shared_ptr<Env>) -> Ptr {
    return pair_car(args);
  });
  S.register_prim("unquote", [](State &S, Ptr args, std::shared_ptr<Env> env) -> Ptr {
    return pair_car(args) ? S.eval(pair_car(args), env) : Ptr();
  });
  S.register_prim("quasiquote", [](State &S, Ptr args, std::shared_ptr<Env> env) -> std::shared_ptr<vdlisp::Value> {
    std::function<Ptr(Ptr, int)> qq_expand = [&](Ptr expr, int depth) -> Ptr {
      if (!expr)
        return {};
      if (is_pair(expr))
      {
        Ptr car = pair_car(expr);
        Ptr cdr = pair_cdr(expr);
        if (is_symbol(car, "unquote"))
        {
          if (depth == 1)
          {
            Ptr uq_args = cdr;
            return uq_args ? S.eval(pair_car(uq_args), env) : Ptr();
          }
          else
          {
            return S.make_pair(car, qq_expand(cdr, depth - 1));
          }
        }
        if (is_symbol(car, "quasiquote"))
        {
          return S.make_pair(car, qq_expand(cdr, depth + 1));
        }
        return S.make_pair(qq_expand(car, depth), qq_expand(cdr, depth));
      }
      return expr;
    };
    return qq_expand(pair_car(args), 1);
  });
  // `if` removed as a primitive; provide it via a macro implemented using `cond`.
  S.register_prim("set", [](State &S, Ptr args, std::shared_ptr<Env> env) -> Ptr {
    Ptr sym = pair_car(args);
    Ptr valexpr = pair_car(pair_cdr(args));
    Ptr val = S.eval(valexpr, env);
    return S.set(sym, val, env);
  });
  S.register_prim("fn", [](State &S, Ptr args, std::shared_ptr<Env> env) -> Ptr {
    Ptr params = pair_car(args);
    Ptr body = pair_cdr(args);
    return S.make_function(params, body, env);
  });
  S.register_prim("macro", [](State &S, Ptr args, std::shared_ptr<Env> env) -> Ptr {
    Ptr params = pair_car(args);
    Ptr body = pair_cdr(args);
    return S.make_macro(params, body, env);
  });
  S.register_prim("let", [](State &S, Ptr args, std::shared_ptr<Env> env) -> Ptr {
    Ptr vars = pair_car(args);
    auto e = std::make_shared<Env>();
    e->parent = env;
    while (vars)
    {
      Ptr sym = pair_car(vars);
      vars = pair_cdr(vars);
      Ptr val = pair_car(vars);
      val = S.eval(val, e);
      S.bind(sym, val, e);
      vars = pair_cdr(vars);
    }
    return S.do_list(pair_cdr(args), e);
  });
  S.register_prim("while", [](State &S, Ptr args, std::shared_ptr<Env> env) -> Ptr {
    Ptr cond = pair_car(args);
    Ptr body = pair_cdr(args);
    Ptr res;
    while (S.eval(cond, env))
    {
      res = S.do_list(body, env);
    }
    return res;
  });
  // cond special form: evaluate clauses sequentially; for the first true
  // test evaluate and return the body. Implemented directly to avoid
  // depending on `if` (which may be provided at the language level as a macro).
  S.register_prim("cond", [](State &S, Ptr args, std::shared_ptr<Env> env) -> Ptr {
    Ptr clauses = args;
    while (clauses) {
      Ptr clause = pair_car(clauses);
      if (!clause) { clauses = pair_cdr(clauses); continue; }
      Ptr test = pair_car(clause);
      Ptr body = pair_cdr(clause);
      Ptr tval = S.eval(test, env);
      if (tval)
        return S.do_list(body, env);
      clauses = pair_cdr(clauses);
    }
    return S.make_nil();
  });

  // Provide `if` as a language-level macro implemented via `cond`.
  // This creates a proper TMACRO binding in the global environment so user
  // scripts can rely on `if` even though it's not a primitive.
  S.register_prim("apply", [](State &S, Ptr args, std::shared_ptr<Env> env) -> Ptr {
    Ptr fnexpr = pair_car(args);
    if (!fnexpr)
      throw std::runtime_error("apply requires a function");
    Ptr listexpr = pair_car(pair_cdr(args));
    Ptr fn = S.eval(fnexpr, env);
    Ptr list = S.eval(listexpr, env);
    return S.call(fn, list, env);
  });
}

} // namespace vdlisp