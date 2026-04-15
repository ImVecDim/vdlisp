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
#include "jit/jit_ir_builder.hpp"

State::State() {
    // 启动时预留常用哈希表容量，减少 REPL 与加载阶段的反复 rehash。
    symbol_intern.reserve(256);
    loaded_modules.reserve(64);
    global = make_env();
    register_core(*this);
    // 语言里把 `#t` 当作 truthy 的全局符号。
    bind_global("#t", make_symbol("#t"));
    // 不额外引入 `else` 关键字，`cond` 默认分支直接写 `#t` 即可。
}

// -------------------- State allocators --------------------

auto State::alloc_string(const std::string &s) -> StringData * {
    return new StringData(s);
}

auto State::alloc_pair(Value &&car, Value &&cdr) -> PairData * {
    auto *p = new PairData();
    // 直接 move 进 pair，可避免临时 Value 带来的引用计数抖动。
    p->car = std::move(car);
    p->cdr = std::move(cdr);
    return p;
}

auto State::alloc_func(Value &&params, Value &&body, Env *env) -> FuncData * {
    FuncData *f = new FuncData();
    // 函数对象捕获参数、函数体和闭包环境，是解释器闭包语义的承载体。
    f->params = std::move(params);
    f->body = std::move(body);
    f->closure_env = env;
    if (env)
        retain_env(env);
    f->num_call_count = 0;
    f->compiled_code = nullptr;
    f->jit_failed = false;
    return f;
}

auto State::alloc_macro(Value &&params, Value &&body, Env *env) -> MacroData * {
    MacroData *m = new MacroData();
    // 宏和函数类似，但调用时绑定的是原始 AST 而不是求值后的参数。
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
    // 大部分局部环境很小，给一个保守初始容量即可。
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
    // 解释器主要靠引用计数回收；退出前主动打断闭包与环境形成的环。
    for (auto &kv : symbol_intern) {
        Value &v = kv.second;
        clear_closure_env(v);
        v = Value();
    }

    // 沿着环境链清空绑定，同时断开 parent 指针，避免层层互相保活。
    if (global) {
        std::vector<Env *> q;
        // 遍历期间先 retain，避免边清理边把链条提前释放掉。
        retain_env(global);
        q.push_back(global);
        for (size_t i = 0; i < q.size(); ++i) {
            auto e = q[i];
            if (!e)
                continue;
            if (e->parent) {
                retain_env(e->parent);
                q.push_back(e->parent);
            }
            for (auto &mkv : e->map) {
                Value &val = mkv.second;
                clear_closure_env(val);
                val = Value();
            }
            e->map.clear();
            if (e->parent) {
                release_env(e->parent);
                e->parent = nullptr;
            }
        }
        for (auto *p : q)
            release_env(p);
    }

    // 其余缓存纯粹是加速结构，直接清空即可。

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
    // 符号做 intern，保证同名 symbol 可按身份快速比较。
    auto it = symbol_intern.find(s);
    if (it != symbol_intern.end()) [[likely]]
        return it->second;
    Value v = make_pooled_value(TSYMBOL);
    v.set_symbol(alloc_string(s));
    symbol_intern[s] = v;
    return v;
}
auto State::make_pair(const Value &car, const Value &cdr) -> Value {
    // 左值版本统一转发到右值版本，保持分配逻辑只有一份。
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
        throw LispError("bind expects a symbol");
    // 局部环境绑定是求值热路径，尽量直接 move 进入 map。
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
            // 原位覆写现有绑定，避免额外 retain/release。
            it->second = std::move(v);
            return v;
        }
        e = e->parent;
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

// 先逐个求值实参，再重新组装成列表交给统一调用入口。
static auto eval_args(State &S, const Value &list, Env *env) -> Value {
    Value head;
    Value *last = &head;
    foreach_lisp(list, [&](const Value &car) {
        *last = S.make_pair(S.eval(car, env), Value());
        last = &(*last).get_pair()->cdr;
    });
    return head;
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
    State::SourceLoc call_loc;
    if (!(S.get_source_loc(S.current_expr, call_loc) || S.get_source_loc(expr, call_loc)))
        return {false, {}};
    call_loc.label = label;
    return {true, {call_loc}};
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
            // 裸 symbol 形参表示“剩余参数整体绑定到这个名字”。
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
    // current_expr 是错误报告的锚点；只有正常完成时才恢复到上一个表达式。
    bool commit = false;    // 用于指示操作是否正常完成或成功提交
    Value prev = std::exchange(current_expr, expr);
    struct Defer {
        State &S; Value &prev; bool &commit;
        ~Defer() { if (commit) S.current_expr = std::move(prev); }
    } defer{*this, prev, commit};

    if (!expr)
        return {};
    if (!env)
        env = global;
    switch (expr.get_type()) {
    case TSYMBOL: {
        // `nil` 既可能表示“查不到”，也可能是“变量值就是 nil”，因此这里必须显式沿环境链查 map。
        auto e = env ? env : global;
        while (e) {
            auto it = e->map.find(*expr.get_symbol());
            if (it != e->map.end()) {
                Value v = it->second;
                commit = true;
                return v;
            }
            e = e->parent;
        }
        {
            State::SourceLoc sl;
            if (get_source_loc(expr, sl)) {
                throw LispError(sl, std::string("unbound symbol: ") + *expr.get_symbol());
            }
        }
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
            commit = true;
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

            commit = true;
            return eval(res, env);
        }
        // 普通函数调用遵循 applicative order：先求值实参，再统一调用。
        Value args = eval_args(*this, cdr, env);
        Value res = call(fn, args, env);
        commit = true;
        return res;
    }
    default:
        commit = true;
        return expr;
    }
}

auto State::call(const Value &fn, const Value &args, Env *env) -> Value {
    (void)env;
    if (!fn) [[unlikely]]
        throw LispError("attempt to call nil");
    if (fn.get_type() == TCFUNC) {
        return fn.get_cfunc()(*this, args);
    } else if (fn.get_type() == TFUNC) {
        // 只有“全部实参都是 number”时，JIT 的 double ABI 才成立。
        FuncData *fd = fn.get_func();
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
            // 以数值调用次数作为简易热点判据，热了才值得尝试编译。
            fd->num_call_count++;
            if (fd->num_call_count > 3 && !fd->compiled_code && !fd->jit_failed) {
                if (!can_attempt_jit_compile(fd)) {
                    fd->jit_failed = true;
                } else {
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
        }

        if (fd && fd->compiled_code && numeric) {
            using JitFn = double (*)(double *, int);
            auto fptr = reinterpret_cast<JitFn>(fd->compiled_code);
            // 进入机器码前挂上当前 State，给桥接函数做回退与自由变量读取。
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
                // NaN 既表示“结果不是纯数值”，也表示“JIT 内部回退失败”，此时单次退回解释器执行。
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

        // 非数值调用、未编译或回退场景都走解释器闭包执行路径。
        const Value &params = fd->params;
        const Value &body = fd->body;
        Env *closure_env = fd->closure_env;
        State::SourceLoc call_loc;
        auto fn_chain = build_call_chain_entry(*this, current_expr, "fn");
        bool have_call_loc = fn_chain.first;
        std::vector<State::SourceLoc> call_chain_entry = std::move(fn_chain.second);
        if (have_call_loc)
            call_loc = call_chain_entry.front();
        return invoke_closure_body(*this, closure_env, params, args, body, /*fill_missing_with_nil=*/false, have_call_loc, call_loc, call_chain_entry);
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
