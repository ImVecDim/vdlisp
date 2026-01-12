#include "nanbox.hpp"
#include "jit/jit.hpp"
#include <iostream>
#include <sstream>
#include <unordered_map>
#include <vector>

using namespace vdlisp;

Env::~Env() noexcept {
    if (parent) {
        release_env(parent);
        parent = nullptr;
    }
}

// JIT compiler instance is provided by `global_jit` declared in the JIT header.
// The concrete `JITCompiler global_jit` definition lives in `src/jit/jit.cpp`.

// -------------------- Value implementation --------------------

Value::Value(Type t) {
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
    // If both Values already contain the same bits (same payload/tag),
    // there's no need to change reference counts or modify state.
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

// Retrieve the function data from the Value object and support JIT trigger.
//
// Behavior:
// - Each call increments `FuncData::call_count`.
// - When `call_count` exceeds a simple threshold (10 here) and the function
//   has not yet been compiled, we request JIT compilation.
// - The JIT integration here is demonstrative: it creates a placeholder
//   `llvm::Function` and calls `JITCompiler::compileFunction`. A real
//   implementation should generate proper IR that matches the function body
//   and calling convention.

void Value::release_payload(Type t, void *p) noexcept {
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
        // Inspect the stored FuncData pointer without triggering JIT or side-effects
        auto *fd = reinterpret_cast<FuncData *>(bits & kPayloadMask);
        return (fd && fd->compiled_code) ? "jit_func" : "function";
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
        // print first element
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
        return (fd && fd->compiled_code) ? "<jit_func>" : "<function>";
    }
    default:
        return "<?>";
    }
}