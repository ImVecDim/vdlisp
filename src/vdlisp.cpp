#include "vdlisp.hpp"
#include <sstream>
#include <cctype>
#include <fstream>
#include <stdexcept>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <filesystem>
#include <unistd.h>
#include <vector>
#include <readline/readline.h>
#include <readline/history.h>
#include <cmath>
#include <new>

using namespace vdlisp;

// -------------------- helpers --------------------

template<class It>
static auto make_string_list_range(State &S, It b, It e) -> Ptr
{
  Ptr head;
  Ptr *last = &head;
  for (; b != e; ++b) {
    *last = S.make_pair(S.make_string(*b), Ptr());
    PairData *pd = (*last)->get_pair();
    last = &pd->cdr;
  }
  return head;
}

#include "helpers.hpp"
#include "core.hpp"
#include "jit/jit.hpp"


State::State()
{
  global = make_env();
  register_core(*this);
  // convenience: bind true symbol '#t'
  bind_global("#t", make_symbol("#t"));
  // bind 'else' as alias for '#t' for cond
  bind_global("else", make_symbol("#t"));
}

// -------------------- State object pool allocators --------------------

auto State::alloc_string(const std::string &s) -> std::string*
{
  std::string *p = string_pool.construct(s);
  return p;
}

auto State::alloc_pair(Ptr car, Ptr cdr) -> PairData*
{
  PairData *p = pair_pool.construct();
  p->car = car;
  p->cdr = cdr;
  return p;
}

auto State::alloc_func(Ptr params, Ptr body, std::shared_ptr<Env> env) -> FuncData*
{
  FuncData *f = func_pool.construct();
  f->params = params;
  f->body = body;
  f->closure_env = env;
  f->call_count = 0;
  f->num_call_count = 0;
  f->compiled_code = nullptr;
  f->jit_failed = false;
  return f;
}

auto State::alloc_macro(Ptr params, Ptr body, std::shared_ptr<Env> env) -> MacroData*
{
  MacroData *m = macro_pool.construct();
  m->params = params;
  m->body = body;
  m->closure_env = env;
  return m;
}

// Value and Env allocators
auto State::alloc_value(Type t) -> Value*
{
  Value *v = value_pool.construct(t);
  return v;
}

auto State::make_pooled_value(Type t) -> Ptr
{
  Value *v = alloc_value(t);
  return Ptr(v, [](Value*){});
}

auto State::alloc_env() -> Env*
{
  Env *e = env_pool.construct();
  e->parent = nullptr;
  e->map.clear();
  return e;
}

auto State::make_env(std::shared_ptr<Env> parent) -> std::shared_ptr<Env>
{
  Env *e = alloc_env();
  e->parent = parent;
  return std::shared_ptr<Env>(e, [](Env*){});
}

void State::shutdown_and_purge_pools()
{
  // 1) Release runtime references so Value destructors can run while pooled
  // objects are still valid.
  env_stack.clear();
  loaded_modules.clear();
  sources.clear();
  src_call_chain_map.clear();
  src_map.clear();
  symbol_intern.clear();
  current_expr.reset();
  global.reset();

  // Best-effort: ask pools to return any completely-free blocks.
  // Note: this may be a no-op if we never individually free chunks.
  (void)macro_pool.purge_memory();
  (void)pair_pool.purge_memory();
  (void)func_pool.purge_memory();
  (void)string_pool.purge_memory();
  (void)value_pool.purge_memory();
  (void)env_pool.purge_memory();

  // IMPORTANT: do NOT destroy/free each object individually here.
  // `boost::object_pool<T>::free/destroy` uses ordered_free with O(N) behavior;
  // doing that per object can become effectively O(N^2) and look like a dead loop.
  // Instead, destroy the entire pool, which performs a single linear scan over blocks.
  // Destruction order matters: keep func_pool alive until after pairs/macros are destroyed,
  // because releasing a function Value consults its FuncData. Also ensure func_pool
  // remains alive while Value destructors run (value_pool destroyed first).
  string_pool.~ExposedObjectPool<std::string>();
  macro_pool.~ExposedObjectPool<MacroData>();
  pair_pool.~ExposedObjectPool<PairData>();
  value_pool.~ExposedObjectPool<Value>();
  func_pool.~ExposedObjectPool<FuncData>();
  env_pool.~ExposedObjectPool<Env>();

  new (&func_pool) ExposedObjectPool<FuncData>();
  new (&value_pool) ExposedObjectPool<Value>();
  new (&pair_pool) ExposedObjectPool<PairData>();
  new (&macro_pool) ExposedObjectPool<MacroData>();
  new (&string_pool) ExposedObjectPool<std::string>();
  new (&env_pool) ExposedObjectPool<Env>();
}

// global used by JIT bridge to access the interpreter State when native
// code needs to fall back to the interpreter.
vdlisp::State* vdlisp::jit_active_state = nullptr;

auto State::make_nil() -> Ptr { return {}; }
auto State::make_number(double n) -> Ptr
{
  Ptr v = make_pooled_value(TNUMBER);
  v->set_number(n);
  return v;
}
auto State::make_string(const std::string &s) -> Ptr
{
  Ptr v = make_pooled_value(TSTRING);
  v->set_string(alloc_string(s));
  return v;
}
auto State::make_symbol(const std::string &s) -> Ptr
{
  auto it = symbol_intern.find(s);
  if (it != symbol_intern.end())
    return it->second;
  Ptr v = make_pooled_value(TSYMBOL);
  v->set_symbol(alloc_string(s));
  symbol_intern[s] = v;
  return v;
}
auto State::make_pair(Ptr car, Ptr cdr) -> Ptr
{
  Ptr v = make_pooled_value(TPAIR);
  v->set_pair(alloc_pair(car, cdr));
  return v;
}
auto State::make_cfunc(const CFunc &fn) -> Ptr
{
  Ptr v = make_pooled_value(TCFUNC);
  v->set_cfunc(fn);
  return v;
}
auto State::make_prim(const Prim &fn) -> Ptr
{
  Ptr v = make_pooled_value(TPRIM);
  v->set_prim(fn);
  return v;
}
auto State::make_function(Ptr params, Ptr body, std::shared_ptr<Env> env) -> Ptr
{
  Ptr v = make_pooled_value(TFUNC);
  v->set_func(alloc_func(params, body, env));
  return v;
}
auto State::make_macro(Ptr params, Ptr body, std::shared_ptr<Env> env) -> Ptr
{
  Ptr v = make_pooled_value(TMACRO);
  v->set_macro(alloc_macro(params, body, env));
  return v;
}

auto State::make_string_list(const std::vector<std::string> &items) -> Ptr
{
  return make_string_list_range(*this, items.begin(), items.end());
}

auto State::make_string_list(int argc, char **argv, int start) -> Ptr
{
  return make_string_list_range(*this, argv + start, argv + argc);
}

auto State::make_string_list(std::initializer_list<std::string> items) -> Ptr
{
  return make_string_list_range(*this, items.begin(), items.end());
}

auto State::make_string_list(std::initializer_list<const char*> items) -> Ptr
{
  return make_string_list_range(*this, items.begin(), items.end());
}

void State::register_builtin(const std::string &name, const CFunc &fn)
{
  bind_global(name, make_cfunc(fn));
}
void State::register_prim(const std::string &name, const Prim &fn)
{
  bind_global(name, make_prim(fn));
}

auto State::bind(Ptr sym, Ptr v, std::shared_ptr<Env> env) -> Ptr
{
  if (!env)
    env = global;
  if (!sym || sym->get_type() != TSYMBOL)
    throw std::runtime_error("bind expects a symbol");
  env->map[*sym->get_symbol()] = v;
  return v;
}

auto State::set(Ptr sym, Ptr v, std::shared_ptr<Env> env) -> Ptr
{
  if (!env)
    env = global;
  std::string key = *sym->get_symbol();
  auto e = env;
  while (e)
  {
    auto it = e->map.find(key);
    if (it != e->map.end())
    {
      it->second = v;
      return v;
    }
    e = e->parent;
  }
  // not found, bind in current env
  bind(sym, v, env);
  return v;
}

void State::bind_global(const std::string &name, Ptr v)
{
  global->map[name] = v;
}

auto State::get_bound(const std::string &name, std::shared_ptr<Env> env) -> Ptr
{
  auto e = env ? env : global;
  while (e)
  {
    auto it = e->map.find(name);
    if (it != e->map.end())
      return it->second;
    e = e->parent;
  }
  return {};
}

// -------------------- parser --------------------

// parser helpers are implemented in `src/helpers.cpp`

// Parse helpers implemented in src/helpers.cpp


// -------------------- eval --------------------

// Evaluate each element of a list and return a new list of evaluated values
static auto eval_args(State &S, Ptr list, std::shared_ptr<Env> env) -> Ptr
{
  Ptr head;
  Ptr *last = &head;
  Ptr a = list;
  while (a)
  {
    Ptr acar = pair_car(a);
    Ptr acdr = pair_cdr(a);
    Ptr av = S.eval(acar, env);
    *last = S.make_pair(av, Ptr());
    PairData *lpd = (*last)->get_pair();
    last = &lpd->cdr;
    a = acdr;
  }
  return head;
}

static void bind_params_to_env(
    std::unordered_map<std::string, Ptr> &out,
    Ptr params,
    Ptr args,
    bool fill_missing_with_nil)
{
  Ptr p = params;
  Ptr a = args;
  while (p)
  {
    if (p && p->get_type() == TSYMBOL) {
      // if params is a bare symbol, bind the rest of args to it
      out[*p->get_symbol()] = a;
      break;
    }

    if (!fill_missing_with_nil && !a)
      break;

    Ptr pcar = pair_car(p);
    Ptr pcdr = pair_cdr(p);

    if (pcar && pcar->get_type() == TSYMBOL)
    {
      Ptr bound = a ? pair_car(a) : Ptr();
      out[*pcar->get_symbol()] = bound;
    }

    p = pcdr;
    a = a ? pair_cdr(a) : Ptr();
  }
}

auto State::eval(Ptr expr, std::shared_ptr<Env> env) -> Ptr
{
  // Keep track of current expression. On exception we leave current_expr set to the
  // failing expression so the top-level can report a source location.
  class EvalContext {
  public:
    EvalContext(State &S, Ptr expr) : S(S), prev(S.current_expr) { S.current_expr = expr; }
    void commit() { commit_flag = true; }
    ~EvalContext() { if (commit_flag) S.current_expr = prev; }

  private:
    State &S;
    Ptr prev;
    bool commit_flag = false;
  } ctx(*this, expr);

  if (!expr)
    return {};
  if (!env)
    env = global;
  switch (expr->get_type())
  {
  case TSYMBOL:
  {
    // Look up symbol in the environment chain while distinguishing
    // between "bound to nil" and "not bound". `get_bound` returns
    // a Ptr which may be null for both cases, so perform lookup here
    // to detect presence in the map.
    auto e = env ? env : global;
    while (e) {
      auto it = e->map.find(*expr->get_symbol());
      if (it != e->map.end()) {
        Ptr v = it->second;
        ctx.commit();
        return v;
      }
      e = e->parent;
    }
    {
      State::SourceLoc sl;
      if (get_source_loc(expr, sl)) {
        throw ParseError(sl, std::string("unbound symbol: ") + *expr->get_symbol());
      }
    }
    throw std::runtime_error("unbound symbol: " + *expr->get_symbol());
  }
  case TPAIR:
  {
    // function application or special form
    PairData *pd = expr->get_pair();
    Ptr car = pd->car;
    Ptr cdr = pd->cdr;
    Ptr fn_expr = car;
    Ptr fn = eval(fn_expr, env);
    if (!fn)
      throw std::runtime_error("attempt to call nil");
    // Special form (prim) receives unevaluated args and env
    if (fn->get_type() == TPRIM)
    {
      Ptr res = fn->get_prim()(*this, cdr, env);
      ctx.commit();
      return res;
    }
    // Macro: bind params to raw args, evaluate body, then evaluate result in caller env
    if (fn->get_type() == TMACRO)
    {
      // bind params to raw args
      MacroData *md = fn->get_macro();
      Ptr params = md->params;
      Ptr body = md->body;
      std::shared_ptr<Env> closure_env = md->closure_env;
      auto e = make_env(closure_env);
      bind_params_to_env(e->map, params, cdr, /*fill_missing_with_nil=*/true);
      // compute call-site location and a one-frame call-chain entry
      State::SourceLoc call_loc;
      bool have_call_loc = (get_source_loc(current_expr, call_loc) || get_source_loc(expr, call_loc));
      std::vector<State::SourceLoc> call_chain_entry;
      if (have_call_loc) {
        if (car && car->get_type() == TSYMBOL)
          call_loc.label = std::string("macro ") + *car->get_symbol();
        else
          call_loc.label = std::string("macro");
        call_chain_entry.push_back(call_loc);
        // If possible, include the macro *definition* location as well so
        // users can see both where the macro was defined and where it was
        // invoked when expansion errors occur.
        State::SourceLoc def_loc;
        if (md && md->body && get_source_loc(md->body, def_loc)) {
          def_loc.label = std::string("macro-def");
          call_chain_entry.push_back(def_loc);
        }
        // record a transient mapping for the call expression itself
        src_call_chain_map[expr.get()] = call_chain_entry;
      }

      Ptr res;
      try {
        res = do_list(body, e);
      }
      catch (const ParseError &pe) {
        if (have_call_loc)
          throw ParseError(call_loc, pe.what(), call_chain_entry);
        throw;
      }
      catch (const std::exception &ex) {
        if (have_call_loc)
          throw ParseError(call_loc, ex.what(), call_chain_entry);
        throw;
      }

      // annotate expanded nodes: set source loc to call site and attach
      // the call-chain (prepending to any existing chain from inner macros)
      if (res && have_call_loc) {
        std::function<void(Ptr)> propagate;
        propagate = [&](Ptr v) -> void {
          if (!v) return;
          set_source_loc(v, call_loc.file, call_loc.line, call_loc.col);
          auto it = src_call_chain_map.find(v.get());
          std::vector<State::SourceLoc> new_chain = call_chain_entry;
          if (it != src_call_chain_map.end()) {
            new_chain.insert(new_chain.end(), it->second.begin(), it->second.end());
          }
          src_call_chain_map[v.get()] = new_chain;
          if (is_pair(v)) {
            propagate(pair_car(v));
            propagate(pair_cdr(v));
          }
        };
        propagate(res);
      }

      ctx.commit();
      return eval(res, env);
    }
    // otherwise evaluate args (for C functions and user functions)
    Ptr args = eval_args(*this, cdr, env);
    Ptr res = call(fn, args, env);
    ctx.commit();
    return res;
  }
  default:
    ctx.commit();
    return expr;
  }
}

auto State::call(Ptr fn, Ptr args, std::shared_ptr<Env> env) -> Ptr
{
  (void)env;
  if (!fn)
    throw std::runtime_error("attempt to call nil");
  if (fn->get_type() == TCFUNC)
  {
    return fn->get_cfunc()(*this, args);
  }
  else if (fn->get_type() == TFUNC)
  {
    // If JIT compiled machine code is available and the arguments are all
    // numeric, call the native code path for performance.
    FuncData *fd = fn->get_func();

    // Check if arguments are all numeric
    std::vector<double> darr;
    Ptr a = args;
    bool numeric = true;
    while (a) {
      Ptr av = pair_car(a);
      if (!av || av->get_type() != TNUMBER) { numeric = false; break; }
      darr.push_back(av->get_number());
      a = pair_cdr(a);
    }

    if (numeric) {
      fd->num_call_count++; // Increment the numeric call count
      // Simple hot-path heuristic: if the function becomes hot with numeric calls, try to compile it.
      if (fd->num_call_count > 3 && !fd->compiled_code && !fd->jit_failed) {
        try {
          void* c = global_jit.compileFuncData(fd);
          if (c) {
            fd->compiled_code = c;
          } else {
            fd->jit_failed = true;
          }
        } catch (...) {
          fd->jit_failed = true;
        }
      }
    }

    if (fd && fd->compiled_code && numeric) {
        using JitFn = double (*)(double*, int);
        auto fptr = reinterpret_cast<JitFn>(fd->compiled_code);
        // set active state so JIT-compiled code can call back into the
        // interpreter when necessary.
        jit_active_state = this;
        double res = 0.0;
        try {
          res = fptr(darr.empty() ? nullptr : darr.data(), (int)darr.size());
        } catch (...) {
          res = std::numeric_limits<double>::quiet_NaN();
        }
        jit_active_state = nullptr;
        if (std::isnan(res)) {
            // callee returned a non-number — disable compiled code and
            // fallback to interpreter implementation for correctness.
            fd->compiled_code = nullptr;
            fd->jit_failed = true;
            Ptr params = fd->params;
            Ptr body = fd->body;
            std::shared_ptr<Env> closure_env = fd->closure_env;
            auto e = make_env(closure_env ? closure_env : global);
            bind_params_to_env(e->map, params, args, /*fill_missing_with_nil=*/false);
            return do_list(body, e);
        }
        return make_number(res);
    }

    // create new env (fallback interpreter path)
    Ptr params = fd->params;
    Ptr body = fd->body;
    std::shared_ptr<Env> closure_env = fd->closure_env;
    auto e = make_env(closure_env ? closure_env : global);
    // bind params (for functions, missing args stop binding as before)
    bind_params_to_env(e->map, params, args, /*fill_missing_with_nil=*/false);
    // evaluate function body with call-chain annotation so errors report the call site
    State::SourceLoc call_loc;
    std::vector<State::SourceLoc> call_chain_entry;
    if (get_source_loc(current_expr, call_loc)) {
      call_loc.label = std::string("fn");
      call_chain_entry.push_back(call_loc);
    }
    try {
      return do_list(body, e);
    }
    catch (const ParseError &pe) {
      if (!call_chain_entry.empty()) {
        std::vector<State::SourceLoc> new_chain = call_chain_entry;
        if (!pe.call_chain.empty()) new_chain.insert(new_chain.end(), pe.call_chain.begin(), pe.call_chain.end());
        throw ParseError(pe.loc, pe.what(), new_chain);
      }
      throw;
    }
    catch (const std::exception &ex) {
      if (!call_chain_entry.empty())
        throw ParseError(call_loc, ex.what(), call_chain_entry);
      throw;
    }
  }
  throw std::runtime_error("not a function");
}

auto State::do_list(Ptr body, std::shared_ptr<Env> env) -> Ptr
{
  Ptr res;
  while (body)
  {
    PairData *pd = body->get_pair();
    Ptr car = pd->car;
    Ptr cdr = pd->cdr;
    res = eval(car, env);
    body = cdr;
  }
  return res;
}

auto State::to_string(Ptr v) -> std::string
{
  if (!v) return "nil";
  return v->to_repr(*this);
}

// Utilities and parsing helpers have been moved to `src/helpers.cpp`.
// The relevant functions now live there: `list_of`, parser helpers, `State::set_source_loc`,
// `State::get_source_loc`, `State::get_source_line`, and `print_error_with_loc`.


// -------------------- main / REPL --------------------

static void print_call_chain(const State &S, const std::vector<State::SourceLoc> &chain)
{
  if (chain.empty()) return;
  std::cerr << "Call chain:\n";
  for (const auto &fr : chain) {
    std::cerr << "  at ";
    if (!fr.label.empty()) std::cerr << fr.label << " ";
    std::cerr << fr.file << ":" << fr.line << ":" << fr.col << "\n";
    std::string line;
    if (S.get_source_line(fr.file, fr.line, line)) {
      std::cerr << "    " << line << "\n";
      size_t col_index = fr.col ? fr.col - 1 : 0;
      std::string caret_spaces;
      for (size_t i = 0; i < col_index; ++i)
        caret_spaces.push_back((i < line.size() && line[i] == '\t') ? '\t' : ' ');
      std::cerr << "    " << caret_spaces << "^" << "\n";
    }
  }
}

static void report_exception(State &S, const std::exception &ex)
{
  if (auto pe = dynamic_cast<const ParseError *>(&ex))
  {
    print_error_with_loc(S, pe->loc, pe->what());
    if (!pe->call_chain.empty()) print_call_chain(S, pe->call_chain);
    return;
  }
  State::SourceLoc loc;
  bool have_loc = S.get_source_loc(S.current_expr, loc);
  if (have_loc)
  {
    print_error_with_loc(S, loc, ex.what());
    // see if the evaluated node has an associated call-chain from prior macro expansion
    auto it = S.src_call_chain_map.find(S.current_expr.get());
    if (it != S.src_call_chain_map.end()) {
      print_call_chain(S, it->second);
    }
  }
  else
  {
    std::cerr << "error: " << ex.what() << "\n";
  }
}

static void repl(State &S)
{
  const char *home = getenv("HOME");
  std::string histfile;
  if (home) histfile = std::string(home) + "/.VDLISP__history";

  if (!histfile.empty())
    read_history(histfile.c_str());

  while (true)
  {
    char *cline = readline("> ");
    if (!cline) break; // EOF (Ctrl-D)
    std::string line(cline);
    free(cline);
    if (line.empty())
      continue;
    add_history(line.c_str());
    try
    {
      Ptr e = S.parse(line);
      if (!e)
        continue;
      Ptr r = S.eval(e, S.global);
      std::cout << S.to_string(r) << "\n";
    }
    catch (const std::exception &ex)
    {
      report_exception(S, ex);
    }
  }

  if (!histfile.empty())
    write_history(histfile.c_str());
}

// Check at runtime that NaN-boxing assumptions hold on this platform.
static auto check_nanboxing_environment() -> bool
{
  // allocate a small object and inspect its pointer bits
  void *p = ::operator new(1);
  auto addr = reinterpret_cast<uint64_t>(p);
  ::operator delete(p);
  // Pointer must fit into the 48-bit payload field
  if ((addr & ~vdlisp::Value::kPayloadMask) != 0)
    return false;
  return true;
}

auto main(int argc, char **argv) -> int
{
  if (!check_nanboxing_environment()) {
    std::cerr << "vdlisp: unsupported platform for NaN-boxing: pointers require more than 48 bits.\n"
              << "This build assumes canonical 48-bit virtual addresses (x86_64)." << std::endl;
    return 1;
  }

  State S;
  // Ensure we return pooled memory on normal exit (helps leak checkers).
  struct ShutdownGuard {
    State &S;
    ~ShutdownGuard() { S.shutdown_and_purge_pools(); }
  } guard{S};
  // bind argv as a list of strings into the global environment
  S.bind_global("argv", S.make_string_list(argc, argv, 1));
  // Auto-load core language helpers implemented in Lisp if supplied.
  try {
    std::filesystem::path langfile("scripts/lang_basics.lisp");
    if (std::filesystem::exists(langfile)) {
      std::ifstream lf(langfile);
      if (lf) {
        std::ostringstream lss;
        lss << lf.rdbuf();
        Ptr le = S.parse_all(lss.str(), langfile.string());
        if (le) S.do_list(le, S.global);
      }
    }
  } catch (...) {
    // ignore failures to auto-load language file
  }
  if (argc < 2)
  {
    repl(S);
    return 0;
  }
  // Load and execute file
  try
  {
    std::ifstream f(argv[1]);
    if (!f)
    {
      std::cerr << "could not open file: " << argv[1] << "\n";
      return 1;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    Ptr e = S.parse_all(ss.str(), argv[1]);
    if (e)
    {
      Ptr r = S.do_list(e, S.global);
      std::cout << S.to_string(r) << "\n";
    }
  }
  catch (const std::exception &ex)
  {
    report_exception(S, ex);
    return 1;
  }
  return 0;
}

















