#ifndef VDLISP__VDLISP__HPP
#define VDLISP__VDLISP__HPP

#include "nanbox.hpp"
#include <boost/pool/object_pool.hpp>
#include <cstddef>
#include <initializer_list>
#include <string>
#include <memory>
#include <vector>
#include <unordered_map>

namespace vdlisp
{

  template <class T>
  struct ExposedObjectPool : boost::object_pool<T>
  {
    using boost::object_pool<T>::purge_memory;
  };

  class State
  {
  public:
    std::vector<std::shared_ptr<Env>> env_stack; // not required, but useful
    std::shared_ptr<Env> global;
    std::unordered_map<std::string, Ptr> symbol_intern;

    State();

    // Release runtime references and return pooled memory to the OS (best-effort).
    // Intended to be called near program exit to make leak checkers' reports cleaner.
    void shutdown_and_purge_pools();

    // factory helpers
    auto make_nil() -> Ptr;
    auto make_number(double n) -> Ptr;
    auto make_string(const std::string &s) -> Ptr;
    auto make_symbol(const std::string &s) -> Ptr;
    auto make_pair(Ptr car, Ptr cdr) -> Ptr;
    auto make_cfunc(const CFunc &fn) -> Ptr;
    auto make_function(Ptr params, Ptr body, std::shared_ptr<Env> env) -> Ptr;
    auto make_prim(const Prim &fn) -> Ptr;
    auto make_macro(Ptr params, Ptr body, std::shared_ptr<Env> env) -> Ptr;

    // convenience helpers for constructing lists
    auto make_string_list(const std::vector<std::string> &items) -> Ptr;
    auto make_string_list(int argc, char **argv, int start = 0) -> Ptr;
    auto make_string_list(std::initializer_list<std::string> items) -> Ptr;
    auto make_string_list(std::initializer_list<const char*> items) -> Ptr;

    // parsing / eval
    auto parse(const std::string &src, const std::string &name = "(string)") -> Ptr;
    auto parse_all(const std::string &src, const std::string &name = "(string)") -> Ptr;
    auto eval(Ptr expr, std::shared_ptr<Env> env) -> Ptr;
    auto call(Ptr fn, Ptr args, std::shared_ptr<Env> env = nullptr) -> Ptr;
    auto do_list(Ptr body, std::shared_ptr<Env> env) -> Ptr;

    // source location helpers
    struct SourceLoc { std::string file; size_t line = 0; size_t col = 0; std::string label; };
    void set_source_loc(Ptr v, const std::string &file, size_t line, size_t col);
    auto get_source_loc(Ptr v, SourceLoc &out) const -> bool;

    // current expression being evaluated (set while evaluating; left set on exception)
    Ptr current_expr;
    // source location map: maps Value* to SourceLoc
    std::unordered_map<const Value*, SourceLoc> src_map;
    // call-chain map for expanded nodes: maps a Value* to the chain of SourceLocs
    // representing macro/function calls that led to this expansion.
    std::unordered_map<const Value*, std::vector<SourceLoc>> src_call_chain_map;

    // source contents per filename
    std::unordered_map<std::string, std::string> sources;
    // cache for required modules: maps canonical filename to result value
    std::unordered_map<std::string, Ptr> loaded_modules;
    // return the indicated line (1-based) from a source file; returns false if not available
    auto get_source_line(const std::string &file, size_t line, std::string &out) const -> bool;

  private:
    // Object pool allocation helpers (avoid raw new/delete)
    auto alloc_string(const std::string &s) -> std::string*;
    auto alloc_pair(Ptr car, Ptr cdr) -> PairData*;
    auto alloc_func(Ptr params, Ptr body, std::shared_ptr<Env> env) -> FuncData*;
    auto alloc_macro(Ptr params, Ptr body, std::shared_ptr<Env> env) -> MacroData*;

    // Boost pools owning runtime objects. Values store raw pointers into these pools.
    ExposedObjectPool<std::string> string_pool;
    ExposedObjectPool<PairData> pair_pool;
    ExposedObjectPool<FuncData> func_pool;
    ExposedObjectPool<MacroData> macro_pool;

  public:
    // helpers
    auto to_string(Ptr v) -> std::string;
    void register_builtin(const std::string &name, const CFunc &fn);
    void register_prim(const std::string &name, const Prim &fn);
    auto get_bound(const std::string &name, std::shared_ptr<Env> env) -> Ptr;
    void bind_global(const std::string &name, Ptr v);
    auto bind(Ptr sym, Ptr v, std::shared_ptr<Env> env) -> Ptr;
    auto set(Ptr sym, Ptr v, std::shared_ptr<Env> env) -> Ptr;


  };

  // Pointer to the currently active State while executing JIT code.
  // Set by `State::call` before entering native JIT code and cleared after.
  extern State* jit_active_state;

  // utility
  auto list_of(State &S, std::initializer_list<Ptr> items) -> Ptr;

} // namespace vdlisp

#endif // VDLISP__VDLISP__HPP
