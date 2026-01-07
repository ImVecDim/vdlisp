#include "vdlisp.hpp"
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <readline/history.h>
#include <readline/readline.h>
#include <sstream>
#include <stdexcept>
#include <unistd.h>
#include <vector>

using namespace vdlisp;

// -------------------- helpers --------------------

// make_string_list helper removed; templated member implemented in `vdlisp.hpp`

#include "core.hpp"
#include "helpers.hpp"
#include "jit/jit.hpp"

State::State() {
    // Pre-reserve common containers to reduce hash-table rehashing
    symbol_intern.reserve(256);
    loaded_modules.reserve(64);
    global = make_env();
    register_core(*this);
    // convenience: bind true symbol '#t'
    bind_global("#t", make_symbol("#t"));
    // Note: do not bind 'else' globally; use `#t` for cond default branch
}

// -------------------- State allocators --------------------

auto State::alloc_string(const std::string &s) -> StringData * {
    return new StringData(s);
}

auto State::alloc_pair(Value &&car, Value &&cdr) -> PairData * {
    auto *p = new PairData();
    // Move values into the pair to avoid extra refcount increments/decrements
    p->car = std::move(car);
    p->cdr = std::move(cdr);
    return p;
}

auto State::alloc_func(Value &&params, Value &&body, Env *env) -> FuncData * {
    FuncData *f = new FuncData();
    // Move parameters/body to avoid extra refcount operations
    f->params = std::move(params);
    f->body = std::move(body);
    f->closure_env = env;
    if (env)
        retain_env(env);
    f->call_count = 0;
    f->num_call_count = 0;
    f->compiled_code = nullptr;
    f->jit_failed = false;
    return f;
}

auto State::alloc_macro(Value &&params, Value &&body, Env *env) -> MacroData * {
    MacroData *m = new MacroData();
    // Move parameters/body to avoid extra refcount operations
    m->params = std::move(params);
    m->body = std::move(body);
    m->closure_env = env;
    if (env)
        retain_env(env);
    return m;
}

// Value and Env allocators
auto State::make_pooled_value(Type t) noexcept -> Value {
    return Value(t);
}

auto State::alloc_env() -> Env * {
    Env *e = new Env();
    e->parent = nullptr;
    e->map.clear();
    // reserve a small default capacity to avoid frequent rehashing for small envs
    e->map.reserve(32);
    return e;
}

auto State::make_env(Env *parent) -> Env * {
    Env *e = alloc_env();
    e->parent = parent;
    if (parent)
        retain_env(parent);
    return e;
}

void State::shutdown_and_purge_pools() {
    // Release runtime references so reference-counted objects can be reclaimed.
    // First: break common cycles that refcounting cannot solve (closures <-> envs).
    // Clear closure envs held by functions/macros in the intern table.
    for (auto &kv : symbol_intern) {
        Value &v = kv.second;
        clear_closure_env(v);
        // Reset the Value to trigger release of referenced payloads
        v = Value();
    }

    // Walk the global environment chain and clear maps / parent pointers
    if (global) {
        std::vector<Env *> q;
        // retain to keep the chain alive while we iterate
        retain_env(global);
        q.push_back(global);
        for (size_t i = 0; i < q.size(); ++i) {
            auto e = q[i];
            if (!e)
                continue;
            // enqueue parent (if any) to ensure we visit the whole chain
            if (e->parent) {
                retain_env(e->parent);
                q.push_back(e->parent);
            }
            // clear function closure_envs for values stored in env maps
            for (auto &mkv : e->map) {
                Value &val = mkv.second;
                clear_closure_env(val);
                val = Value();
            }
            e->map.clear();
            // release the child's hold on its parent (the parent itself is retained in `q`)
            if (e->parent) {
                release_env(e->parent);
                e->parent = nullptr;
            }
        }
        // release retained queue entries
        for (auto *p : q)
            release_env(p);
    }

    // Clear other runtime caches and containers

    if (global) {
        release_env(global);
        global = nullptr;
    }

    for (auto &kv : loaded_modules)
        kv.second = Value();
    loaded_modules.clear();

    sources.clear();
    src_call_chain_map.clear();
    src_map.clear();

    symbol_intern.clear();
    current_expr = Value();
}

// global used by JIT bridge to access the interpreter State when native
// code needs to fall back to the interpreter.
vdlisp::State *vdlisp::jit_active_state = nullptr;

auto State::make_nil() noexcept -> Value {
    return {};
}
auto State::make_number(double n) noexcept -> Value {
    Value v = make_pooled_value(TNUMBER);
    v.set_number(n);
    return v;
}
auto State::make_string(const std::string &s) -> Value {
    Value v = make_pooled_value(TSTRING);
    v.set_string(alloc_string(s));
    return v;
}
auto State::make_symbol(const std::string &s) -> Value {
    auto it = symbol_intern.find(s);
    if (it != symbol_intern.end()) [[likely]]
        return it->second;
    Value v = make_pooled_value(TSYMBOL);
    v.set_symbol(alloc_string(s));
    symbol_intern[s] = v;
    return v;
}
auto State::make_pair(const Value &car, const Value &cdr) -> Value {
    // copy into temporaries and forward to rvalue overload
    return make_pair(Value(car), Value(cdr));
}

auto State::make_pair(Value &&car, Value &&cdr) -> Value {
    Value v = make_pooled_value(TPAIR);
    v.set_pair(alloc_pair(std::move(car), std::move(cdr)));
    return v;
}
auto State::make_cfunc(const CFunc &fn) noexcept -> Value {
    Value v = make_pooled_value(TCFUNC);
    v.set_cfunc(fn);
    return v;
}
auto State::make_prim(const Prim &fn) noexcept -> Value {
    Value v = make_pooled_value(TPRIM);
    v.set_prim(fn);
    return v;
}
auto State::make_function(const Value &params, const Value &body, Env *env) -> Value {
    return make_function(Value(params), Value(body), env);
}

auto State::make_function(Value &&params, Value &&body, Env *env) -> Value {
    Value v = make_pooled_value(TFUNC);
    v.set_func(alloc_func(std::move(params), std::move(body), env));
    return v;
}
auto State::make_macro(const Value &params, const Value &body, Env *env) -> Value {
    return make_macro(Value(params), Value(body), env);
}

auto State::make_macro(Value &&params, Value &&body, Env *env) -> Value {
    Value v = make_pooled_value(TMACRO);
    v.set_macro(alloc_macro(std::move(params), std::move(body), env));
    return v;
}

auto State::make_string_list(int argc, char **argv, int start) -> Value {
    return make_string_list(argv + start, argv + argc);
}

void State::register_builtin(const std::string &name, const CFunc &fn) {
    bind_global(name, make_cfunc(fn));
}
void State::register_prim(const std::string &name, const Prim &fn) {
    bind_global(name, make_prim(fn));
}

auto State::bind(const Value &sym, Value v, Env *env) -> Value {
    if (!env)
        env = global;
    if (!sym || sym.get_type() != TSYMBOL)
        throw std::runtime_error("bind expects a symbol");
    // Move into the map to avoid incrementing/decrementing refcounts unnecessarily
    env->map[*sym.get_symbol()] = std::move(v);
    return v;
}

auto State::set(const Value &sym, Value v, Env *env) -> Value {
    if (!env)
        env = global;
    std::string key = *sym.get_symbol();
    auto e = env;
    while (e) {
        auto it = e->map.find(key);
        if (it != e->map.end()) [[likely]] {
            // Move into the existing slot to avoid extra retain/release
            it->second = std::move(v);
            return v;
        }
        e = e->parent;
    }
    // not found, bind in current env
    (void)bind(sym, std::move(v), env);
    return v;
}

void State::bind_global(const std::string &name, Value v) {
    // Move the temporary into the map to avoid a redundant copy/retain
    global->map[name] = std::move(v);
}

auto State::get_bound(const std::string &name, Env *env) -> Value {
    auto e = env ? env : global;
    while (e) {
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
static auto eval_args(State &S, const Value &list, Env *env) -> Value {
    Value head;
    Value *last = &head;
    const Value *a = &list;
    while (*a) {
        PairData *apd = a->get_pair();
        const Value &acar = apd->car;
        const Value &acdr = apd->cdr;
        Value av = S.eval(acar, env);
        *last = S.make_pair(std::move(av), Value());
        PairData *lpd = (*last).get_pair();
        last = &lpd->cdr;
        a = &acdr;
    }
    return head;
}

// Helper to run a callable and uniformly annotate/rethrow errors with a
// call-chain entry when a call-site location is available.
template <typename Fn>
static auto with_call_chain(State &S, bool have_call_loc, const State::SourceLoc &call_loc, const std::vector<State::SourceLoc> &call_chain_entry, Fn &&fn) -> Value {
    try {
        return fn();
    } catch (const ParseError &pe) {
        if (have_call_loc) {
            std::vector<State::SourceLoc> new_chain = call_chain_entry;
            if (!pe.call_chain.empty())
                new_chain.insert(new_chain.end(), pe.call_chain.begin(), pe.call_chain.end());
            throw ParseError(pe.loc, pe.what(), new_chain);
        }
        throw;
    } catch (const std::exception &ex) {
        if (have_call_loc)
            throw ParseError(call_loc, ex.what(), call_chain_entry);
        throw;
    }
}

static void bind_params_to_env(
    std::unordered_map<std::string, Value> &out,
    const Value &params,
    const Value &args,
    bool fill_missing_with_nil) {
    const Value *p = &params;
    const Value *a = &args;
    while (*p) {
        if (p->get_type() == TSYMBOL) {
            // if params is a bare symbol, bind the rest of args to it
            out[*p->get_symbol()] = *a;
            break;
        }

        if (!fill_missing_with_nil && !*a)
            break;

        PairData *ppd = p->get_pair();
        const Value &pcar = ppd->car;
        const Value &pcdr = ppd->cdr;

        if (pcar && pcar.get_type() == TSYMBOL) {
            // Avoid an extra temporary Value copy: assign directly into the map.
            if (*a) {
                PairData *apd = a->get_pair();
                out[*pcar.get_symbol()] = apd->car;
            } else {
                out[*pcar.get_symbol()] = Value{};
            }
        }

        p = &pcdr;
        if (*a) {
            PairData *apd = a->get_pair();
            a = &apd->cdr;
        }
    }
}

auto State::eval(const Value &expr, Env *env) -> Value {
    // Keep track of current expression. On exception we leave current_expr set to the
    // failing expression so the top-level can report a source location.
    class EvalContext {
      public:
        EvalContext(State &S, const Value &expr) : S(S), prev(std::move(S.current_expr)) {
            S.current_expr = expr;
        }
        void commit() {
            commit_flag = true;
        }
        ~EvalContext() {
            if (commit_flag)
                std::swap(S.current_expr, prev);
        }

      private:
        State &S;
        Value prev;
        bool commit_flag = false;
    } ctx(*this, expr);

    if (!expr)
        return {};
    if (!env)
        env = global;
    switch (expr.get_type()) {
    case TSYMBOL: {
        // Look up symbol in the environment chain while distinguishing
        // between "bound to nil" and "not bound". `get_bound` returns
        // a Value which may be null for both cases, so perform lookup here
        // to detect presence in the map.
        auto e = env ? env : global;
        while (e) {
            auto it = e->map.find(*expr.get_symbol());
            if (it != e->map.end()) {
                Value v = it->second;
                ctx.commit();
                return v;
            }
            e = e->parent;
        }
        {
            State::SourceLoc sl;
            if (get_source_loc(expr, sl)) {
                throw ParseError(sl, std::string("unbound symbol: ") + *expr.get_symbol());
            }
        }
        throw std::runtime_error("unbound symbol: " + *expr.get_symbol());
    }
    case TPAIR: {
        // function application or special form
        PairData *pd = expr.get_pair();
        const Value &car = pd->car;
        const Value &cdr = pd->cdr;
        Value fn = eval(car, env);
        if (!fn)
            throw std::runtime_error("attempt to call nil");
        // Special form (prim) receives unevaluated args and env
        if (fn.get_type() == TPRIM) {
            Value res = fn.get_prim()(*this, cdr, env);
            ctx.commit();
            return res;
        }
        // Macro: bind params to raw args, evaluate body, then evaluate result in caller env
        if (fn.get_type() == TMACRO) {
            // bind params to raw args
            MacroData *md = fn.get_macro();
            const Value &params = md->params;
            const Value &body = md->body;
            Env *closure_env = md->closure_env;
            Env *e = make_env(closure_env);
            EnvGuard eg(e);
            bind_params_to_env(e->map, params, cdr, /*fill_missing_with_nil=*/true);
            // compute call-site location and a one-frame call-chain entry
            State::SourceLoc call_loc;
            bool have_call_loc = (get_source_loc(current_expr, call_loc) || get_source_loc(expr, call_loc));
            std::vector<State::SourceLoc> call_chain_entry;
            if (have_call_loc) {
                if (car && car.get_type() == TSYMBOL)
                    call_loc.label = std::string("macro ") + *car.get_symbol();
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
                src_call_chain_map[expr.identity_key()] = call_chain_entry;
            }

            Value res = with_call_chain(*this, have_call_loc, call_loc, call_chain_entry, [&]() -> Value {
                return do_list(body, e);
            });

            // annotate expanded nodes: set source loc to call site and attach
            // the call-chain (prepending to any existing chain from inner macros)
            if (res && have_call_loc) {
                std::function<void(const Value &)> propagate;
                propagate = [&](const Value &v) -> void {
                    if (!v)
                        return;
                    set_source_loc(v, call_loc.file, call_loc.line, call_loc.col);
                    auto it = src_call_chain_map.find(v.identity_key());
                    std::vector<State::SourceLoc> new_chain = call_chain_entry;
                    if (it != src_call_chain_map.end()) {
                        new_chain.insert(new_chain.end(), it->second.begin(), it->second.end());
                    }
                    src_call_chain_map[v.identity_key()] = new_chain;
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
        Value args = eval_args(*this, cdr, env);
        Value res = call(fn, args, env);
        ctx.commit();
        return res;
    }
    default:
        ctx.commit();
        return expr;
    }
}

auto State::call(const Value &fn, const Value &args, Env *env) -> Value {
    (void)env;
    if (!fn) [[unlikely]]
        throw std::runtime_error("attempt to call nil");
    if (fn.get_type() == TCFUNC) {
        return fn.get_cfunc()(*this, args);
    } else if (fn.get_type() == TFUNC) {
        // If JIT compiled machine code is available and the arguments are all
        // numeric, call the native code path for performance.
        FuncData *fd = fn.get_func();
        // Check if arguments are all numeric
        std::vector<double> darr;
        const Value *a = &args;
        bool numeric = true;
        while (*a) {
            PairData *apd = a->get_pair();
            const Value &av = apd->car;
            if (!av || av.get_type() != TNUMBER) {
                numeric = false;
                break;
            }
            darr.push_back(av.get_number());
            a = &apd->cdr;
        }

        if (numeric) {
            fd->num_call_count++; // Increment the numeric call count
            // Simple hot-path heuristic: if the function becomes hot with numeric calls, try to compile it.
            if (fd->num_call_count > 3 && !fd->compiled_code && !fd->jit_failed) {
                try {
                    void *c = global_jit.compileFuncData(fd);
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
            using JitFn = double (*)(double *, int);
            auto fptr = reinterpret_cast<JitFn>(fd->compiled_code);
            // set active state so JIT-compiled code can call back into the
            // interpreter when necessary.
            jit_active_state = this;
            double res = 0.0;
            bool jit_threw = false;
            try {
                res = fptr(darr.empty() ? nullptr : darr.data(), (int)darr.size());
            } catch (...) {
                jit_threw = true;
                res = std::numeric_limits<double>::quiet_NaN();
            }
            jit_active_state = nullptr;
            if (std::isnan(res)) {
                // Deopt: callee returned a non-number (signaled as NaN).
                // This can happen transiently (e.g. a free variable becomes non-numeric).
                // Fall back to the interpreter for this call, but do not permanently
                // disable JIT unless the compiled code itself threw.
                if (jit_threw) {
                    fd->compiled_code = nullptr;
                    fd->jit_failed = true;
                }
                const Value &params = fd->params;
                const Value &body = fd->body;
                Env *closure_env = fd->closure_env;
                Env *e = make_env(closure_env ? closure_env : global);
                EnvGuard eg(e);
                bind_params_to_env(e->map, params, args, /*fill_missing_with_nil=*/false);
                return do_list(body, e);
            }
            return make_number(res);
        }

        // create new env (fallback interpreter path)
        const Value &params = fd->params;
        const Value &body = fd->body;
        Env *closure_env = fd->closure_env;
        Env *e = make_env(closure_env ? closure_env : global);
        EnvGuard eg(e);
        // bind params (for functions, missing args stop binding as before)
        bind_params_to_env(e->map, params, args, /*fill_missing_with_nil=*/false);
        // evaluate function body with call-chain annotation so errors report the call site
        State::SourceLoc call_loc;
        std::vector<State::SourceLoc> call_chain_entry;
        if (get_source_loc(current_expr, call_loc)) {
            call_loc.label = std::string("fn");
            call_chain_entry.push_back(call_loc);
        }
        bool have_call_loc = !call_chain_entry.empty();
        return with_call_chain(*this, have_call_loc, call_loc, call_chain_entry, [&]() -> Value {
            return do_list(body, e);
        });
    }
    throw std::runtime_error("not a function");
}

auto State::do_list(const Value &body, Env *env) -> Value {
    const Value *walk = &body;
    Value res;
    while (*walk) {
        PairData *pd = walk->get_pair();
        const Value &car = pd->car;
        const Value &cdr = pd->cdr;
        res = eval(car, env);
        walk = &cdr;
    }
    return res;
}

auto State::to_string(const Value &v) -> std::string {
    if (!v)
        return "nil";
    return v.to_repr(*this);
}

// Utilities and parsing helpers have been moved to `src/helpers.cpp`.
// The relevant functions now live there: `list_of`, parser helpers, `State::set_source_loc`,
// `State::get_source_loc`, `State::get_source_line`, and `print_error_with_loc`.

// NOTE: `main` and REPL-related helpers have been moved to `src/main.cpp`.
