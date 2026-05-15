#ifndef VDLISP__NANBOX_HPP
#define VDLISP__NANBOX_HPP

#include <bit>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace vdlisp {

class Value;
class PairData;
class StringData;
class FuncData;
class MacroData;
class State;
class Env;

using Prim = Value (*)(State &, const Value &, Env *);
using CFunc = Value (*)(State &, const Value &);

enum Type : uint8_t {
    TNIL, TPAIR, TNUMBER, TSTRING, TSYMBOL,
    TFUNC, TMACRO, TPRIM, TCFUNC
};

// Forward declarations needed for the implementation
namespace detail {
inline constexpr auto double_to_bits(double value) noexcept -> uint64_t { return std::bit_cast<uint64_t>(value); }
inline constexpr auto bits_to_double(uint64_t bits) noexcept -> double { return std::bit_cast<double>(bits); }
} // namespace detail

struct RcBase {
    size_t refs_{1};
    RcBase() noexcept = default;
    ~RcBase() noexcept = default;
    RcBase(const RcBase &) = delete;
    RcBase &operator=(const RcBase &) = delete;

    inline __attribute__((always_inline)) void inc_ref() noexcept { ++refs_; }
    inline __attribute__((always_inline)) size_t dec_ref() noexcept { return --refs_; }
};

class StringData : public RcBase {
  public:
    explicit StringData(std::string_view s) : value(s) {}
    std::string value;
};

class Env : public RcBase {
  public:
        // 一个环境就是“当前作用域绑定表 + 指向父作用域的链”。
    std::unordered_map<std::string, Value> map;
    Env *parent = nullptr;
    ~Env() noexcept;
};

// Env 单独提供 retain/release，避免和普通 Value 的引用计数细节混在一起。
inline __attribute__((always_inline)) void retain_env(Env *e) noexcept {
    if (e)
        e->inc_ref();
}
inline __attribute__((always_inline)) void release_env(Env *e) noexcept {
    if (e && e->dec_ref() == 0)
        delete e;
}

struct EnvGuard {
    explicit EnvGuard(Env *e = nullptr) noexcept : e_{e} {}
    ~EnvGuard() { if (e_) release_env(e_); }
    EnvGuard(const EnvGuard &) = delete;
    EnvGuard &operator=(const EnvGuard &) = delete;
    EnvGuard(EnvGuard &&o) noexcept : e_(std::exchange(o.e_, nullptr)) {}
    EnvGuard &operator=(EnvGuard &&o) noexcept {
        if (this != &o) {
            release_env(e_);
            e_ = std::exchange(o.e_, nullptr);
        }
        return *this;
    }

  private:
    Env *e_;
};

class Value {
  public:
        // Value 用 NaN-boxing 把 number、指针对象和函数指针全塞进一个 64 位槽里。
    static constexpr uint64_t kNaNMask = 0x7FF0000000000000ULL;
    static constexpr uint64_t kTagMask = kNaNMask | 0x000F000000000000ULL; // NaN + tag bits
    static constexpr uint64_t kPayloadMask = 0x0000FFFFFFFFFFFFULL;        // 48 bits for payload

    // 4 bit tag 决定 payload 里装的到底是哪一种运行时对象。
    static constexpr uint64_t kTagNil = kNaNMask | 0x0000000000000000ULL;
    static constexpr uint64_t kTagPair = kNaNMask | 0x0001000000000000ULL;
    static constexpr uint64_t kTagString = kNaNMask | 0x0002000000000000ULL;
    static constexpr uint64_t kTagSymbol = kNaNMask | 0x0003000000000000ULL;
    static constexpr uint64_t kTagFunc = kNaNMask | 0x0004000000000000ULL;
    static constexpr uint64_t kTagMacro = kNaNMask | 0x0005000000000000ULL;
    static constexpr uint64_t kTagPrim = kNaNMask | 0x0006000000000000ULL;
    static constexpr uint64_t kTagCFunc = kNaNMask | 0x0007000000000000ULL;

    Value() : bits(kTagNil) {}
    explicit Value(Type t);
    Value(std::nullptr_t) : bits(kTagNil) {}
    Value(const Value &other);
    Value(Value &&other) noexcept;
    ~Value();

    auto operator=(const Value &other) noexcept -> Value &;
    auto operator=(Value &&other) noexcept -> Value &;
    auto operator=(std::nullptr_t) noexcept -> Value &;

    // Getters

    // get_type 是解释器最热的函数之一，先走 number 快路径。
    [[nodiscard]] inline auto get_type() const noexcept -> Type {
        if ((bits & kNaNMask) != kNaNMask) [[unlikely]]
            return TNUMBER;
        static constexpr Type kTagMap[16] = {
            TNIL, TPAIR, TSTRING, TSYMBOL,
            TFUNC, TMACRO, TPRIM, TCFUNC,
            TNIL, TNIL, TNIL, TNIL,
            TNIL, TNIL, TNIL, TNIL};
        return kTagMap[(bits >> 48) & 0xF];
    }
    [[nodiscard]] auto get_number() const noexcept -> double;
    [[nodiscard]] auto get_pair() const noexcept -> PairData *;
    [[nodiscard]] auto get_string() const noexcept -> std::string *;
    [[nodiscard]] auto get_symbol() const noexcept -> std::string *;
    [[nodiscard]] auto get_func() const noexcept -> FuncData *;
    [[nodiscard]] auto get_macro() const noexcept -> MacroData *;
    [[nodiscard]] auto get_prim() const noexcept -> Prim;
    [[nodiscard]] auto get_cfunc() const noexcept -> CFunc;

    [[nodiscard]] explicit operator bool() const noexcept { return get_type() != TNIL; }
    [[nodiscard]] auto operator==(std::nullptr_t) const noexcept -> bool { return get_type() == TNIL; }
    [[nodiscard]] auto operator!=(std::nullptr_t) const noexcept -> bool { return get_type() != TNIL; }
    [[nodiscard]] auto operator==(const Value &rhs) const noexcept -> bool { return bits == rhs.bits; }
    [[nodiscard]] auto operator!=(const Value &rhs) const noexcept -> bool { return bits != rhs.bits; }
    [[nodiscard]] auto identity_key() const noexcept -> uint64_t { return bits; }
    auto reset() noexcept -> void { *this = Value(); }

    // 高层辅助接口主要用于错误信息和打印。
    [[nodiscard]] auto type_name() const -> std::string;
    auto to_repr(State &S) const -> std::string;

    // set_* 负责改写 Value 的类型与 payload，并处理必要的释放。
    auto set_number(double value) noexcept -> void;
    auto set_pair(PairData *ptr) noexcept -> void;
    auto set_string(StringData *ptr) noexcept -> void;
    auto set_symbol(StringData *ptr) noexcept -> void;
    auto set_func(FuncData *ptr) noexcept -> void;
    auto set_macro(MacroData *ptr) noexcept -> void;
    auto set_prim(Prim fn) noexcept -> void;
    auto set_cfunc(CFunc fn) noexcept -> void;

  private:
    auto retain() const noexcept -> void;
    auto release() noexcept -> void;
    [[nodiscard]] auto payload_ptr() const noexcept -> void * { return reinterpret_cast<void *>(bits & kPayloadMask); }
    static auto retain_payload(Type t, void *p) noexcept -> void;
    static auto release_payload(Type t, void *p) noexcept -> void;
    static auto is_refcounted(Type t) noexcept -> bool;

    // 这些模板帮助统一实现“tag + payload”的读写，不把位操作散到各处。
    template <uint64_t Tag, typename DataT>
    inline auto get_payload_raw() const noexcept -> DataT *;

    template <uint64_t Tag, typename DataT>
    inline void set_payload_raw(DataT *ptr) noexcept;

    template <uint64_t Tag, typename Fn>
    inline auto get_fn_raw() const noexcept -> Fn;

    template <uint64_t Tag, typename Fn>
    inline void set_fn_raw(Fn fn) noexcept;

    // 所有 Lisp 值最终都编码到这里。
    uint64_t bits;
};

// 热路径上的短方法直接内联，避免解释器在 Value 访问上损失过多开销。
inline auto Value::get_number() const noexcept -> double {
    double result;
    static_assert(sizeof(double) == sizeof(bits), "Double must be 64-bit");
    std::memcpy(&result, &bits, sizeof(result));
    return result;
}

inline auto Value::set_number(double value) noexcept -> void {
    release();
    std::memcpy(&bits, &value, sizeof(bits));
    // NaN bit-pattern collides with NaN-boxing tags, so we can't represent NaN.
    // Silently convert to 0.0 to maintain type-system invariants.
    if ((bits & kNaNMask) == kNaNMask)
        bits = 0;
}

// 模板定义放在头文件里，保证编译期可见并保持内联机会。
template <uint64_t Tag, typename DataT>
inline __attribute__((always_inline)) auto Value::get_payload_raw() const noexcept -> DataT * { return reinterpret_cast<DataT *>(bits & kPayloadMask); }

template <uint64_t Tag, typename DataT>
inline void Value::set_payload_raw(DataT *ptr) noexcept {
    uint64_t newp = reinterpret_cast<uint64_t>(ptr) & kPayloadMask;
    if (((bits & kTagMask) == Tag) && ((bits & kPayloadMask) == newp))
        return;
    release();
    bits = Tag | newp;
}

template <uint64_t Tag, typename Fn>
inline auto Value::get_fn_raw() const noexcept -> Fn {
    Fn fn;
    uint64_t payload = bits & kPayloadMask;
    std::memcpy(&fn, &payload, sizeof(fn));
    return fn;
}

template <uint64_t Tag, typename Fn>
inline auto Value::set_fn_raw(Fn fn) noexcept -> void {
    release();
    uint64_t payload = 0;
    std::memcpy(&payload, &fn, sizeof(fn));
    bits = Tag | (payload & kPayloadMask);
}

inline __attribute__((always_inline)) auto Value::get_pair() const noexcept -> PairData * { return get_payload_raw<kTagPair, PairData>(); }
inline auto Value::set_pair(PairData *ptr) noexcept -> void { set_payload_raw<kTagPair, PairData>(ptr); }

inline __attribute__((always_inline)) auto Value::get_string() const noexcept -> std::string * {
    auto *sd = get_payload_raw<kTagString, StringData>();
    return sd ? &sd->value : nullptr;
}
inline auto Value::set_string(StringData *ptr) noexcept -> void { set_payload_raw<kTagString, StringData>(ptr); }

inline __attribute__((always_inline)) auto Value::get_symbol() const noexcept -> std::string * {
    auto *sd = get_payload_raw<kTagSymbol, StringData>();
    return sd ? &sd->value : nullptr;
}
inline auto Value::set_symbol(StringData *ptr) noexcept -> void { set_payload_raw<kTagSymbol, StringData>(ptr); }

inline auto Value::get_func() const noexcept -> FuncData * { return get_payload_raw<kTagFunc, FuncData>(); }
inline auto Value::set_func(FuncData *ptr) noexcept -> void { set_payload_raw<kTagFunc, FuncData>(ptr); }

inline auto Value::get_macro() const noexcept -> MacroData * { return get_payload_raw<kTagMacro, MacroData>(); }
inline auto Value::set_macro(MacroData *ptr) noexcept -> void { set_payload_raw<kTagMacro, MacroData>(ptr); }

inline auto Value::get_prim() const noexcept -> Prim { return get_fn_raw<kTagPrim, Prim>(); }
inline auto Value::set_prim(Prim fn) noexcept -> void { set_fn_raw<kTagPrim, Prim>(fn); }

inline auto Value::get_cfunc() const noexcept -> CFunc { return get_fn_raw<kTagCFunc, CFunc>(); }
inline auto Value::set_cfunc(CFunc fn) noexcept -> void { set_fn_raw<kTagCFunc, CFunc>(fn); }

inline __attribute__((always_inline)) auto Value::retain() const noexcept -> void {
    Type t = get_type();
    if (!is_refcounted(t))
        return;
    retain_payload(t, payload_ptr());
}

inline __attribute__((always_inline)) auto Value::release() noexcept -> void {
    Type t = get_type();
    if (!is_refcounted(t))
        return;
    release_payload(t, payload_ptr());
    bits = kTagNil;
}

inline auto Value::is_refcounted(Type t) noexcept -> bool {
    // 哪些类型需要引用计数是编译期常量，直接查表即可。
    constexpr bool kIsRefcounted[] = {
        /*TNIL*/ false,
        /*TPAIR*/ true,
        /*TNUMBER*/ false,
        /*TSTRING*/ true,
        /*TSYMBOL*/ true,
        /*TFUNC*/ true,
        /*TMACRO*/ true,
        /*TPRIM*/ false,
        /*TCFUNC*/ false};
    size_t idx = static_cast<size_t>(t);
    return idx < (sizeof(kIsRefcounted) / sizeof(kIsRefcounted[0])) ? kIsRefcounted[idx] : false;
}

inline __attribute__((always_inline)) void Value::retain_payload(Type t, void *p) noexcept {
    if (p)
        static_cast<RcBase *>(p)->inc_ref();
}

class PairData : public RcBase {
  public:
        // PairData 是 Lisp 链表与 AST 的基础节点。
    Value car;
    Value cdr;

    // 配对分配器使用 slab，operator delete 无需释放内存。
    static void operator delete(void *p) noexcept {}
};

// FuncData 保存闭包求值所需的全部运行时信息，也是 JIT 的编译单元。
class FuncData : public RcBase {
  public:
    Value params;
    Value body;
    Env *closure_env = nullptr;
    size_t num_call_count = 0;
    void *compiled_code = nullptr;
    bool jit_failed = false;
    bool compiling = false; // 防止递归 JIT 编译形成环
};

// 宏只参与展开，不走 JIT，因此结构比函数更简单。
class MacroData : public RcBase {
  public:
    Value params;
    Value body;
    Env *closure_env = nullptr;
};

} // namespace vdlisp

#endif // VDLISP__NANBOX_HPP