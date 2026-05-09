#include "nanbox.hpp"
#include "jit/jit.hpp"
#include <iostream>
#include <sstream>
#include <utility>

using namespace vdlisp;

namespace {

// 对外显示时，热点数值函数如果已经编译或足够热，就显示成 jit_func。
static auto shows_as_jit_func(const FuncData *fd) -> bool {
    return fd && (fd->compiled_code || fd->num_call_count > 3);
}

} // namespace

Env::~Env() noexcept {
    // 父环境由子环境持有一份引用，这里负责对称释放。
    if (parent) {
        release_env(parent);
        parent = nullptr;
    }
}

// JIT compiler instance is provided by `global_jit` declared in the JIT header.
// The concrete `JITCompiler global_jit` definition lives in `src/jit/jit.cpp`.

// -------------------- Value implementation --------------------

Value::Value(Type t) {
    static constexpr uint64_t kTagTab[] = {
        kTagNil, kTagPair, 0, kTagString, kTagSymbol,
        kTagFunc, kTagMacro, kTagPrim, kTagCFunc,
    };
    bits = kTagTab[t];
    // TNUMBER 编码为 IEEE754 0.0（即 uint64 0）
}

Value::Value(const Value &other) : bits(other.bits) {
    retain();
}

Value::Value(Value &&other) noexcept : bits(other.bits) {
    other.bits = kTagNil;
}

Value::~Value() {
    release();
}

auto Value::operator=(const Value &other) noexcept -> Value & {
    if (this != &other && bits != other.bits) {
        other.retain();
        release();
        bits = other.bits;
    }
    return *this;
}

auto Value::operator=(Value &&other) noexcept -> Value & {
    release();
    bits = std::exchange(other.bits, kTagNil);
    return *this;
}

auto Value::operator=(std::nullptr_t) noexcept -> Value & {
    release();
    bits = kTagNil;
    return *this;
}

void Value::release_payload(Type t, void *p) noexcept {
    if (!p) return;
    if (static_cast<RcBase *>(p)->dec_ref() != 0) return;
    switch (t) {
    case TPAIR: delete static_cast<PairData *>(p); break;
    case TSTRING: [[fallthrough]];
    case TSYMBOL: delete static_cast<StringData *>(p); break;
    case TFUNC: {
        auto *fd = static_cast<FuncData *>(p);
        if (fd->compiled_code) { global_jit.releaseFunctionCode(fd->compiled_code); fd->compiled_code = nullptr; }
        if (fd->closure_env) { release_env(fd->closure_env); fd->closure_env = nullptr; }
        delete fd;
        break;
    }
    case TMACRO: delete static_cast<MacroData *>(p); break;
    default: break;
    }
}

// High-level helpers centralized on Value
auto Value::type_name() const -> std::string {
    // 对函数额外区分 jit_func，便于调试当前是否已经进入热点编译。
    static constexpr const char *kNames[] = {
        "nil",      // TNIL
        "pair",     // TPAIR
        "number",   // TNUMBER
        "string",   // TSTRING
        "symbol",   // TSYMBOL
        "function", // TFUNC (special-cased below)
        "macro",    // TMACRO
        "prim",     // TPRIM
        "cfunction" // TCFUNC
    };
    Type t = get_type();
    if (t == TFUNC) {
        // 这里只读取元数据，不触发任何额外行为。
        auto *fd = reinterpret_cast<FuncData *>(bits & kPayloadMask);
        return shows_as_jit_func(fd) ? "jit_func" : "function";
    }
    return kNames[t];
}

auto Value::to_repr(State &S) const -> std::string {
    if (get_type() == TNUMBER) {
        std::ostringstream ss;
        ss << get_number();
        return ss.str();
    }
    switch (get_type()) {
    case TSTRING:
        return *get_string();
    case TSYMBOL:
        return *get_symbol();
    case TPAIR: {
        std::string s = "(";
        // pair 既用于普通 cons cell，也用于打印形如 (a b . c) 的表结构。
        PairData *pd = get_pair();
        if (pd) {
            s += pd->car ? pd->car.to_repr(S) : std::string("nil");
            Value cur = pd->cdr;
            while (cur && cur.get_type() == TPAIR) {
                s += " ";
                PairData *cpd = cur.get_pair();
                s += cpd->car ? cpd->car.to_repr(S) : std::string("nil");
                cur = cpd->cdr;
            }
            if (cur) {
                s += " . ";
                s += cur.to_repr(S);
            }
        }
        s += ")";
        return s;
    }
    case TCFUNC:
        return "<cfunc>";
    case TMACRO:
        return "<macro>";
    case TPRIM:
        return "<prim>";
    case TFUNC: {
        auto *fd = reinterpret_cast<FuncData *>(bits & kPayloadMask);
        return shows_as_jit_func(fd) ? "<jit_func>" : "<function>";
    }
    default:
        return "<?>";
    }
}