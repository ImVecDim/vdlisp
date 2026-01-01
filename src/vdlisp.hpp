#ifndef VDLISP__VDLISP__HPP
#define VDLISP__VDLISP__HPP

#include "nanbox.hpp"
#include <cstddef>
#include <initializer_list>
#include <string>
#include <vector>
#include <unordered_map>

namespace vdlisp
{

  class State
  {
  public:
    Env *global = nullptr;
    std::unordered_map<std::string, Value> symbol_intern;

    State();

    // Release runtime references (best-effort).
    void shutdown_and_purge_pools();

    // factory helpers
    auto make_nil() -> Value;
    auto make_number(double n) -> Value;
    auto make_string(const std::string &s) -> Value;
    auto make_symbol(const std::string &s) -> Value;
    auto make_pair(Value car, Value cdr) -> Value;
    auto make_cfunc(const CFunc &fn) -> Value;
    auto make_function(Value params, Value body, Env *env) -> Value;
    auto make_prim(const Prim &fn) -> Value;
    auto make_macro(Value params, Value body, Env *env) -> Value;

    // pooled helpers
    auto make_pooled_value(Type t) -> Value;
    auto make_env(Env *parent = nullptr) -> Env*;

    // convenience helpers for constructing lists
    auto make_string_list(const std::vector<std::string> &items) -> Value;
    auto make_string_list(int argc, char **argv, int start = 0) -> Value;
    auto make_string_list(std::initializer_list<std::string> items) -> Value;
    auto make_string_list(std::initializer_list<const char*> items) -> Value;

    // parsing / eval
    auto parse(const std::string &src, const std::string &name = "(string)") -> Value;
    auto parse_all(const std::string &src, const std::string &name = "(string)") -> Value;
    auto eval(const Value &expr, Env *env) -> Value;
    auto call(const Value &fn, const Value &args, Env *env = nullptr) -> Value;
    auto do_list(const Value &body, Env *env) -> Value;

    // source location helpers
    struct SourceLoc { std::string file; size_t line = 0; size_t col = 0; std::string label; };
    void set_source_loc(const Value &v, const std::string &file, size_t line, size_t col);
    auto get_source_loc(const Value &v, SourceLoc &out) const -> bool;

    // current expression being evaluated (set while evaluating; left set on exception)
    Value current_expr;
    // source location map: maps Value* to SourceLoc
    std::unordered_map<uint64_t, SourceLoc> src_map;
    // call-chain map for expanded nodes: maps a Value* to the chain of SourceLocs
    // representing macro/function calls that led to this expansion.
    std::unordered_map<uint64_t, std::vector<SourceLoc>> src_call_chain_map;

    // source contents per filename
    std::unordered_map<std::string, std::string> sources;
    // cache for required modules: maps canonical filename to result value
    std::unordered_map<std::string, Value> loaded_modules;
    // return the indicated line (1-based) from a source file; returns false if not available
    auto get_source_line(const std::string &file, size_t line, std::string &out) const -> bool;

  private:
    // Allocation helpers
    auto alloc_string(const std::string &s) -> StringData*;
    auto alloc_pair(Value car, Value cdr) -> PairData*;
    auto alloc_func(Value params, Value body, Env *env) -> FuncData*;
    auto alloc_macro(Value params, Value body, Env *env) -> MacroData*;

    // Pooled allocation helpers for Value and Env
    auto alloc_env() -> Env*;

  public:
    // helpers
    auto to_string(const Value &v) -> std::string;
    void register_builtin(const std::string &name, const CFunc &fn);
    void register_prim(const std::string &name, const Prim &fn);
    auto get_bound(const std::string &name, Env *env) -> Value;
    void bind_global(const std::string &name, Value v);
    auto bind(const Value &sym, Value v, Env *env) -> Value;
    auto set(const Value &sym, Value v, Env *env) -> Value;


  };

  // Pointer to the currently active State while executing JIT code.
  // Set by `State::call` before entering native JIT code and cleared after.
  extern State* jit_active_state;

  // utility
  auto list_of(State &S, std::initializer_list<Value> items) -> Value;

} // namespace vdlisp

#endif // VDLISP__VDLISP__HPP
