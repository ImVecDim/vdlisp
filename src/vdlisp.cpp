#include "vdlisp.hpp"
#include <cmath>
#include <functional>
#include <limits>
#include <vector>

using namespace vdlisp;

// -------------------- helpers --------------------

// make_string_list helper removed; templated member implemented in `vdlisp.hpp`

#include "lib/lib.hpp"
#include "helpers.hpp"
#include "jit/jit.hpp"
#include "jit/jit_ir_builder.hpp"

State::State() {
    // 启动时预留常用哈希表容量，减少 REPL 与加载阶段的反复 rehash。
    symbol_intern.reserve(256);
    loaded_modules.reserve(64);
    global = make_env();
    register_lib(*this);
    // 语言里把 `#t` 当作 truthy 的全局符号。
    bind_global("#t", make_symbol("#t"));
    // 不额外引入 `else` 关键字，`cond` 默认分支直接写 `#t` 即可。
}

// -------------------- State allocators --------------------

auto State::alloc_string(const std::string &s) -> StringData * {
    return new StringData(s);
}

auto State::alloc_pair(Value &&car, Value &&cdr) -> PairData * {
    auto *p = pair_pool.alloc();
    // 直接 move 进 pair，可避免临时 Value 带来的引用计数抖动。
    p->car = std::move(car);
    p->cdr = std::move(cdr);
    return p;
}

auto State::alloc_func(Value &&params, Value &&body, Env *env) -> FuncData * {
    auto *f = new FuncData();
    f->params = std::move(params);
    f->body = std::move(body);
    f->closure_env = env;
    if (env) retain_env(env);
    f->num_call_count = 0;
    return f;
}

auto State::alloc_macro(Value &&params, Value &&body, Env *env) -> MacroData * {
    auto *m = new MacroData();
    m->params = std::move(params);
    m->body = std::move(body);
    m->closure_env = env;
    if (env) retain_env(env);
    return m;
}

// Value and Env allocators

auto State::alloc_env() -> Env * {
    auto *e = new Env();
    e->parent = nullptr;
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

auto State::shutdown_and_purge_pools() -> void {
    // 退出前主动打断闭包与环境形成的环。
    for (auto &kv : symbol_intern) {
        clear_closure_env(kv.second);
        kv.second = Value();
    }

    // 递归清空环境链
    std::function<void(Env *)> clear_env;
    clear_env = [&](Env *e) -> void {
        if (!e) return;
        if (e->parent) {
            retain_env(e->parent);
            clear_env(e->parent);
            release_env(e->parent);
        }
        for (auto &mkv : e->map) {
            clear_closure_env(mkv.second);
            mkv.second = Value();
        }
        e->map.clear();
        e->parent = nullptr;
    };
    if (global) {
        retain_env(global);
        clear_env(global);
        release_env(global);
    }

    pair_pool.purge();

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

// JIT 代码需要回退到解释器时，会通过这个全局指针找到当前 State。
vdlisp::State *vdlisp::jit_active_state = nullptr;

auto State::make_nil() noexcept -> Value {
    return {};
}
auto State::make_number(double n) noexcept -> Value {
    Value v{TNUMBER};
    v.set_number(n);
    return v;
}
auto State::make_string(const std::string &s) -> Value {
    Value v{TSTRING};
    v.set_string(alloc_string(s));
    return v;
}
auto State::make_symbol(const std::string &s) -> Value {
    // 符号做 intern，保证同名 symbol 可按身份快速比较。
    auto it = symbol_intern.find(s);
    if (it != symbol_intern.end()) [[likely]]
        return it->second;
    Value v{TSYMBOL};
    v.set_symbol(alloc_string(s));
    symbol_intern[s] = v;
    return v;
}
auto State::make_pair(const Value &car, const Value &cdr) -> Value {
    // 左值版本统一转发到右值版本，保持分配逻辑只有一份。
    return make_pair(Value(car), Value(cdr));
}

auto State::make_pair(Value &&car, Value &&cdr) -> Value {
    Value v{TPAIR};
    v.set_pair(alloc_pair(std::move(car), std::move(cdr)));
    return v;
}
auto State::make_cfunc(const CFunc &fn) noexcept -> Value {
    Value v{TCFUNC};
    v.set_cfunc(fn);
    return v;
}
auto State::make_prim(const Prim &fn) noexcept -> Value {
    Value v{TPRIM};
    v.set_prim(fn);
    return v;
}
auto State::make_function(const Value &params, const Value &body, Env *env) -> Value {
    return make_function(Value(params), Value(body), env);
}

auto State::make_function(Value &&params, Value &&body, Env *env) -> Value {
    Value v{TFUNC};
    v.set_func(alloc_func(std::move(params), std::move(body), env));
    return v;
}
auto State::make_macro(const Value &params, const Value &body, Env *env) -> Value {
    return make_macro(Value(params), Value(body), env);
}

auto State::make_macro(Value &&params, Value &&body, Env *env) -> Value {
    Value v{TMACRO};
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
        throw LispError("bind expects a symbol");
    // 局部环境绑定是求值热路径，尽量直接 move 进入 map。
    env->map[*sym.get_symbol()] = std::move(v);
    return v;
}

auto State::set(const Value &sym, Value v, Env *env) -> Value {
    if (!env)
        env = global;
    if (!sym || sym.get_type() != TSYMBOL)
        throw LispError("set expects a symbol");
    if (auto *vp = lookup(*sym.get_symbol(), env)) [[likely]] {
        *vp = std::move(v);
        return v;
    }
    // 向上找不到时，退化为在当前环境创建新绑定。
    (void)bind(sym, std::move(v), env);
    return v;
}

void State::bind_global(const std::string &name, Value v) {
    // 全局绑定同样直接 move，避免多余的引用计数操作。
    global->map[name] = std::move(v);
}

auto State::get_bound(const std::string &name, Env *env) -> Value {
    if (auto *vp = lookup(name, env))
        return *vp;
    return {};
}

auto State::lookup(const std::string &name, Env *env) -> Value * {
    auto e = env ? env : global;
    while (e) {
        auto it = e->map.find(name);
        if (it != e->map.end())
            return &it->second;
        e = e->parent;
    }
    return nullptr;
}

// -------------------- parser --------------------

// parser helpers are implemented in `src/helpers.cpp`

// Parse helpers implemented in src/helpers.cpp

// -------------------- eval --------------------

// 先逐个求值实参，再重新组装成列表交给统一调用入口。
static auto eval_args(State &S, const Value &list, Env *env) -> Value {
    ListBuilder lb;
    foreach_lisp(list, [&](const Value &car) {
        lb.add(S, S.eval(car, env));
    });
    return std::move(lb).done();
}

// 包装一次调用，把调用点位置信息一致地附着到异常上。
template <typename Fn>
static auto with_call_chain(State &S, bool have_call_loc, const State::SourceLoc &call_loc, const std::vector<State::SourceLoc> &call_chain_entry, Fn &&fn) -> Value {
    try {
        return fn();
    } catch (const LispError &le) {
        if (have_call_loc) {
            std::vector<State::SourceLoc> new_chain = call_chain_entry;
            if (!le.call_chain.empty())
                new_chain.insert(new_chain.end(), le.call_chain.begin(), le.call_chain.end());
            if (le.has_loc) {
                throw LispError(le.loc, le.what(), new_chain);
            } else {
                throw LispError(call_loc, le.what(), new_chain);
            }
        }
        throw;
    } catch (const std::exception &ex) {
        if (have_call_loc)
            throw LispError(call_loc, ex.what(), call_chain_entry);
        throw;
    }
}

static void bind_params_to_env(
    std::unordered_map<std::string, Value> &out,
    const Value &params,
    const Value &args,
    bool fill_missing_with_nil);

static auto build_call_chain_entry(State &S, const Value &expr, const char *label) -> std::pair<bool, std::vector<State::SourceLoc>> {
    State::SourceLoc loc;
    if (!S.get_source_loc(S.current_expr, loc) && !S.get_source_loc(expr, loc))
        return {false, {}};
    loc.label = label;
    return {true, {std::move(loc)}};
}

static auto invoke_closure_body(
    State &S,
    Env *closure_env,
    const Value &params,
    const Value &args,
    const Value &body,
    bool fill_missing_with_nil,
    bool have_call_loc,
    const State::SourceLoc &call_loc,
    const std::vector<State::SourceLoc> &call_chain_entry) -> Value {
    // 调用函数/宏时总是创建一个新环境，把实参绑定写进去后执行函数体。
    Env *e = S.make_env(closure_env ? closure_env : S.global);
    EnvGuard eg(e);
    bind_params_to_env(e->map, params, args, fill_missing_with_nil);
    return with_call_chain(S, have_call_loc, call_loc, call_chain_entry, [&]() -> Value {
        return S.do_list(body, e);
    });
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
            // 裸 symbol 形参表示"剩余参数整体绑定到这个名字"。
            out[*p->get_symbol()] = *a;
            break;
        }

        if (!fill_missing_with_nil && !*a)
            break;

        PairData *ppd = p->get_pair();
        const Value &pcar = ppd->car;
        const Value &pcdr = ppd->cdr;

        if (pcar && pcar.get_type() == TSYMBOL) {
            // 逐个把形参与实参头部对齐绑定进局部环境。
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
    Value prev = std::exchange(current_expr, expr);
    bool committed = false;
    struct Guard { State &S; Value &prev; bool &c; ~Guard() { if (c) S.current_expr = std::move(prev); } } guard{*this, prev, committed};

    if (!expr)
        return {};
    if (!env)
        env = global;
    switch (expr.get_type()) {
    case TSYMBOL: {
        if (auto *vp = lookup(*expr.get_symbol(), env)) {
            committed = true;
            return *vp;
        }
        State::SourceLoc sl;
        if (get_source_loc(expr, sl))
            throw LispError(sl, std::string("unbound symbol: ") + *expr.get_symbol());
        throw LispError("unbound symbol: " + *expr.get_symbol());
    }
    case TPAIR: {
        // pair 在求值阶段要么是调用表达式，要么是特殊形式/宏展开入口。
        PairData *pd = expr.get_pair();
        const Value &car = pd->car;
        const Value &cdr = pd->cdr;
        Value fn = eval(car, env);
        if (!fn)
            throw LispError("attempt to call nil");
        // 特殊形式自行控制参数求值时机。
        if (fn.get_type() == TPRIM) {
            Value res = fn.get_prim()(*this, cdr, env);
            committed = true;
            return res;
        }
        // 宏先拿到原始 AST，展开后再把展开结果放回调用点环境继续求值。
        if (fn.get_type() == TMACRO) {
            MacroData *md = fn.get_macro();
            const Value &params = md->params;
            const Value &body = md->body;
            Env *closure_env = md->closure_env;
            // 记录宏调用点与宏定义点，方便把展开错误串成一条调用链。
            State::SourceLoc call_loc;
            std::vector<State::SourceLoc> call_chain_entry;
            auto macro_chain = build_call_chain_entry(*this, expr, "macro");
            bool have_call_loc = macro_chain.first;
            if (have_call_loc) {
                call_loc = macro_chain.second.front();
                if (car && car.get_type() == TSYMBOL)
                    call_loc.label = std::string("macro ") + *car.get_symbol();
                call_chain_entry = macro_chain.second;
                call_chain_entry.front() = call_loc;
                State::SourceLoc def_loc;
                if (md && md->body && get_source_loc(md->body, def_loc)) {
                    def_loc.label = std::string("macro-def");
                    call_chain_entry.push_back(def_loc);
                }
                src_call_chain_map[expr.identity_key()] = call_chain_entry;
            }

            Value res = invoke_closure_body(*this, closure_env, params, cdr, body, /*fill_missing_with_nil=*/true, have_call_loc, call_loc, call_chain_entry);

            // 展开结果里的每个节点都继承调用点位置，这样后续报错能回到宏调用处。
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

            committed = true;
            return eval(res, env);
        }
        // 普通函数调用遵循 applicative order：先求值实参，再统一调用。
        Value args = eval_args(*this, cdr, env);
        Value res = call(fn, args, env);
        committed = true;
        return res;
    }
    default:
        committed = true;
        return expr;
    }
}

namespace {
// 从 args 列表中提取实参的 double 值，失败返回 false。
static auto extract_numeric_args(const Value &args, std::vector<double> &out) -> bool {
    for (auto a = &args; *a; a = &a->get_pair()->cdr) {
        const Value &av = a->get_pair()->car;
        if (!av || av.get_type() != TNUMBER) return false;
        out.push_back(av.get_number());
    }
    return true;
}

// 尝试对 FuncData 做 JIT 编译
static auto attempt_jit_compile(FuncData *fd) -> void {
    if (!fd || fd->jit_failed || fd->compiled_code || fd->num_call_count <= 3) return;
    try {
        void *c = global_jit.compileFuncData(fd);
        fd->compiled_code = c;
        if (!c) fd->jit_failed = true;
    } catch (...) { fd->jit_failed = true; }
}

// 统一的闭包调用入口（含调用链追踪）
static auto invoke_func(State &S, FuncData *fd, const Value &args) -> Value {
    auto [have_call_loc, call_chain_entry] = build_call_chain_entry(S, S.current_expr, "fn");
    State::SourceLoc call_loc = have_call_loc ? call_chain_entry.front() : State::SourceLoc{};
    return invoke_closure_body(S, fd->closure_env, fd->params, args, fd->body, false,
                                have_call_loc, call_loc, call_chain_entry);
}
} // namespace

auto State::call(const Value &fn, const Value &args, Env *env) -> Value {
    (void)env;
    if (!fn) [[unlikely]]
        throw LispError("attempt to call nil");
    if (fn.get_type() == TCFUNC) {
        return fn.get_cfunc()(*this, args);
    } else if (fn.get_type() == TFUNC) {
        FuncData *fd = fn.get_func();
        std::vector<double> darr;
        bool numeric = extract_numeric_args(args, darr);

        if (numeric) {
            fd->num_call_count++;
            attempt_jit_compile(fd);
        }

        if (fd->compiled_code && numeric) {
            using JitFn = double (*)(double *, int);
            jit_active_state = this;
            double res = 0.0;
            bool jit_threw = false;
            try {
                res = reinterpret_cast<JitFn>(fd->compiled_code)(
                    darr.empty() ? nullptr : darr.data(), (int)darr.size());
            } catch (...) {
                jit_threw = true;
                res = std::numeric_limits<double>::quiet_NaN();
            }
            jit_active_state = nullptr;
            if (std::isnan(res)) {
                if (jit_threw) { fd->compiled_code = nullptr; fd->jit_failed = true; }
                return invoke_func(*this, fd, args);
            }
            return make_number(res);
        }

        return invoke_func(*this, fd, args);
    }
    throw LispError("not a function");
}

auto State::do_list(const Value &body, Env *env) -> Value {
    // 顺序执行 body，返回最后一个表达式的结果。
    Value res;
    foreach_lisp(body, [&](const Value &car) { res = eval(car, env); });
    return res;
}

auto State::to_string(const Value &v) -> std::string {
    if (!v)
        return "nil";
    return v.to_repr(*this);
}

// 解析辅助与 REPL 入口都被拆到其他编译单元，以便这里聚焦运行时主逻辑。
