#include "nanbox.hpp"
#include "jit/jit.hpp"
#include <iostream>
#include <sstream>
#include <unordered_map>
#include <vector>

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
    // 构造时只建立类型标签，不在这里分配实际 payload。
    switch (t) {
    case TNIL:
        bits = kTagNil;
        break;
    case TNUMBER:
        bits = 0; // 0.0 as IEEE754
        break;
    case TPAIR:
        bits = kTagPair;
        break;
    case TSTRING:
        bits = kTagString;
        break;
    case TSYMBOL:
        bits = kTagSymbol;
        break;
    case TFUNC:
        bits = kTagFunc;
        break;
    case TMACRO:
        bits = kTagMacro;
        break;
    case TPRIM:
        bits = kTagPrim;
        break;
    case TCFUNC:
        bits = kTagCFunc;
        break;
    default:
        bits = kTagNil;
        break;
    }
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

#include <utility>

auto Value::operator=(const Value &other) noexcept -> Value & {
    if (this == &other)
        return *this;
    // bits 完全相同代表底层对象相同，无需动引用计数。
    if (bits == other.bits)
        return *this;
    other.retain();
    release();
    bits = other.bits;
    return *this;
}

auto Value::operator=(Value &&other) noexcept -> Value & {
    if (this == &other)
        return *this;
    release();
    bits = other.bits;
    other.bits = kTagNil;
    return *this;
}

auto Value::operator=(std::nullptr_t) noexcept -> Value & {
    release();
    bits = kTagNil;
    return *this;
}

void Value::release_payload(Type t, void *p) noexcept {
    // 真正的对象析构集中在这里，确保引用计数降到 0 后按类型清理。
    if (!p)
        return;
    auto *rc = static_cast<RcBase *>(p);
    if (rc->dec_ref() != 0)
        return;

    switch (t) {
    case TPAIR:
        delete static_cast<PairData *>(p);
        break;
    case TSTRING:
        delete static_cast<StringData *>(p);
        break;
    case TSYMBOL:
        delete static_cast<StringData *>(p);
        break;
    case TFUNC: {
        auto *fd = static_cast<FuncData *>(p);
        // 函数值销毁时还要回收它占用的机器码与闭包环境。
        if (fd->compiled_code) {
            global_jit.releaseFunctionCode(fd->compiled_code);
            fd->compiled_code = nullptr;
        }
        if (fd->closure_env) {
            release_env(fd->closure_env);
            fd->closure_env = nullptr;
        }
        delete fd;
        break;
    }
    case TMACRO:
        delete static_cast<MacroData *>(p);
        break;
    default:
        break;
    }
}

// High-level helpers centralized on Value
auto Value::type_name() const -> std::string {
    // 对函数额外区分 jit_func，便于调试当前是否已经进入热点编译。
    switch (get_type()) {
    case TNIL:
        return "nil";
    case TPAIR:
        return "pair";
    case TNUMBER:
        return "number";
    case TSTRING:
        return "string";
    case TSYMBOL:
        return "symbol";
    case TFUNC: {
        // 这里只读取元数据，不触发任何额外行为。
        auto *fd = reinterpret_cast<FuncData *>(bits & kPayloadMask);
        return shows_as_jit_func(fd) ? "jit_func" : "function";
    }
    case TMACRO:
        return "macro";
    case TPRIM:
        return "prim";
    case TCFUNC:
        return "cfunction";
    default:
        return "?";
    }
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