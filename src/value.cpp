#include "vdlisp.hpp"
#include <charconv>
#include <iostream>
#include <utility>

using namespace vdlisp;

namespace {

// release_payload 的销毁函数表，用查表代替 switch 分发。
// 环境释放已移入 ClosureData 析构，此处统一 delete。
using Destructor = void (*)(void *p) noexcept;
template <typename T>
static void destroy(void *p) noexcept { delete static_cast<T *>(p); }
constexpr std::array<Destructor, 9> kDestructors = {
    nullptr,              // TNIL
    destroy<PairData>,    // TPAIR
    nullptr,              // TNUMBER
    destroy<StringData>,  // TSTRING
    destroy<StringData>,  // TSYMBOL
    destroy<FuncData>,    // TFUNC
    destroy<MacroData>,   // TMACRO
    nullptr,              // TPRIM
    nullptr,              // TCFUNC
};

} // namespace

// -------------------- Value implementation --------------------

Value::Value(Type t) {
    static constexpr std::array kTagTab = {
        kTagNil, kTagPair, 0ULL, kTagString, kTagSymbol,
        kTagFunc, kTagMacro, kTagPrim, kTagCFunc,
    };
    bits = kTagTab[static_cast<size_t>(t)];
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
    size_t idx = static_cast<size_t>(t);
    if (idx < std::size(kDestructors) && kDestructors[idx])
        kDestructors[idx](p);
}

// High-level helpers centralized on Value
auto Value::type_name() const noexcept -> std::string_view {
    static constexpr std::array kNames = {
        std::string_view{"nil"},       // TNIL
        std::string_view{"pair"},      // TPAIR
        std::string_view{"number"},    // TNUMBER
        std::string_view{"string"},    // TSTRING
        std::string_view{"symbol"},    // TSYMBOL
        std::string_view{"function"},  // TFUNC
        std::string_view{"macro"},     // TMACRO
        std::string_view{"prim"},      // TPRIM
        std::string_view{"cfunction"}, // TCFUNC
    };
    return kNames[static_cast<size_t>(get_type())];
}

auto Value::to_repr(State &S, std::string &out) const -> void {
    Type t = get_type();
    if (t == TNUMBER) {
        std::array<char, 64> buf;
        auto [end, ec] = std::to_chars(buf.data(), buf.data() + buf.size(), get_number());
        out.append(buf.data(), end - buf.data());
        return;
    }
    switch (t) {
    case TSTRING:
        out += *get_string();
        return;
    case TSYMBOL:
        out += *get_symbol();
        return;
    case TPAIR: {
        out += '(';
        PairData *pd = get_pair();
        if (pd) {
            if (pd->car) {
                pd->car.to_repr(S, out);
            } else {
                out += "nil";
            }
            Value cur = pd->cdr;
            while (cur && cur.get_type() == TPAIR) {
                out += ' ';
                PairData *cpd = cur.get_pair();
                if (cpd->car) {
                    cpd->car.to_repr(S, out);
                } else {
                    out += "nil";
                }
                cur = cpd->cdr;
            }
            if (cur) {
                out += " . ";
                cur.to_repr(S, out);
            }
        }
        out += ')';
        return;
    }
    case TCFUNC: out += "<cfunc>"; return;
    case TMACRO: out += "<macro>"; return;
    case TPRIM:  out += "<prim>"; return;
    case TFUNC:  out += "<function>"; return;
    default: std::unreachable();
    }
}
