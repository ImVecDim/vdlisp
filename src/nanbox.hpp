#ifndef VDLISP__NANBOX_HPP
#define VDLISP__NANBOX_HPP

#include <bit>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>

namespace vdlisp {

class Value;
class PairData;
class StringData;
class FuncData;
class MacroData;
class State;
class Env;
// using Value = Value;

// Use plain function pointer types for primitives and C functions to avoid
// the overhead of std::function (allocations / type-erasure / indirect calls).
using Prim = Value (*)(State &, const Value &, Env *);
using CFunc = Value (*)(State &, const Value &);

enum Type {
    TNIL,
    TPAIR,
    TNUMBER,
    TSTRING,
    TSYMBOL,
    TFUNC,  // user function
    TMACRO, // macro
    TPRIM,  // special form (unevaluated args)
    TCFUNC  // c++ builtin
};

// Forward declarations needed for the implementation
namespace detail {
inline constexpr auto double_to_bits(double value) noexcept -> uint64_t { return std::bit_cast<uint64_t>(value); }
inline constexpr auto bits_to_double(uint64_t bits) noexcept -> double { return std::bit_cast<double>(bits); }
} // namespace detail

struct RcBase {
  protected:
    RcBase(size_t init = 1) noexcept : refs_{init} {}
    ~RcBase() noexcept = default;

  private:
    size_t refs_{1};

  public:
    inline __attribute__((always_inline)) void inc_ref() noexcept { ++refs_; }
    inline __attribute__((always_inline)) size_t dec_ref() noexcept { return --refs_; }
    inline __attribute__((always_inline)) size_t ref_count() const noexcept { return refs_; }
};

class StringData : public RcBase {
  public:
    explicit StringData(const std::string &s) : value(s) {}
    std::string value;
};

class Env : public RcBase {
  public:
    std::unordered_map<std::string, Value> map;
    Env *parent = nullptr;
    ~Env();
};

// Helpers to manage Env reference counts (centralized for clarity)
inline __attribute__((always_inline)) void retain_env(Env *e) noexcept {
    if (e)
        e->inc_ref();
}
inline __attribute__((always_inline)) void release_env(Env *e) noexcept {
    if (e && e->dec_ref() == 0)
        delete e;
}

// RAII guard that owns a temporary Env* reference and releases it on destruction.
struct EnvGuard {
    explicit EnvGuard(Env *e = nullptr) noexcept : e_{e} {}
    ~EnvGuard() {
        if (e_)
            release_env(e_);
    }
    EnvGuard(const EnvGuard &) = delete;
    EnvGuard &operator=(const EnvGuard &) = delete;
    EnvGuard(EnvGuard &&o) noexcept : e_(o.e_) {
        o.e_ = nullptr;
    }
    EnvGuard &operator=(EnvGuard &&o) noexcept {
        if (this != &o) {
            if (e_)
                release_env(e_);
            e_ = o.e_;
            o.e_ = nullptr;
        }
        return *this;
    }
    [[nodiscard]] Env *get() const noexcept { return e_; }
    [[nodiscard]] Env *release() noexcept {
        Env *t = e_;
        e_ = nullptr;
        return t;
    }

  private:
    Env *e_;
};

class Value {
  public:
    // IEEE 754 double format: 1 sign bit + 11 exponent bits + 52 mantissa bits
    // For NaN: exponent is all 1s (0x7FF)
    static constexpr uint64_t kNaNMask = 0x7FF0000000000000ULL;
    static constexpr uint64_t kTagMask = kNaNMask | 0x000F000000000000ULL; // NaN + tag bits
    static constexpr uint64_t kPayloadMask = 0x0000FFFFFFFFFFFFULL;        // 48 bits for payload

    // Tags for different pointer types
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

    // Hot function, inlined for performance. Use branch hint for the common numeric path.
    [[nodiscard]] inline auto get_type() const noexcept -> Type {
        // Fast path for non-NaN values (numbers)
        if ((bits & kNaNMask) != kNaNMask) [[unlikely]]
            return TNUMBER;

        // Use the 4-bit tag stored in bits[48:51] to index into a small
        // constexpr lookup table. This keeps the check compact and avoids
        // a large switch at runtime.
        constexpr Type kTagMap[16] = {
            /*0*/ TNIL, /*1*/ TPAIR, /*2*/ TSTRING, /*3*/ TSYMBOL,
            /*4*/ TFUNC, /*5*/ TMACRO, /*6*/ TPRIM, /*7*/ TCFUNC,
            /*8*/ TNIL, /*9*/ TNIL, /*10*/ TNIL, /*11*/ TNIL,
            /*12*/ TNIL, /*13*/ TNIL, /*14*/ TNIL, /*15*/ TNIL};
        uint8_t idx = static_cast<uint8_t>((bits >> 48) & 0xF);
        return kTagMap[idx];
    }
    [[nodiscard]] auto get_number() const noexcept -> double;
    [[nodiscard]] auto get_pair() const noexcept -> PairData *;
    [[nodiscard]] auto get_string() const noexcept -> std::string *;
    [[nodiscard]] auto get_symbol() const noexcept -> std::string *;
    [[nodiscard]] auto get_func() const noexcept -> FuncData *;
    [[nodiscard]] auto get_macro() const noexcept -> MacroData *;
    [[nodiscard]] Prim get_prim() const noexcept;
    [[nodiscard]] CFunc get_cfunc() const noexcept;

    //[[nodiscard]] inline auto operator->() -> Value* { return this; }
    //[[nodiscard]] inline auto operator->() const -> const Value* { return this; }
    //[[nodiscard]] inline auto get() -> Value* { return this; }
    //[[nodiscard]] inline auto get() const -> const Value* { return this; }
    [[nodiscard]] explicit operator bool() const noexcept { return get_type() != TNIL; }
    [[nodiscard]] auto operator==(std::nullptr_t) const noexcept -> bool { return get_type() == TNIL; }
    [[nodiscard]] auto operator!=(std::nullptr_t) const noexcept -> bool { return get_type() != TNIL; }
    [[nodiscard]] auto operator==(const Value &rhs) const noexcept -> bool { return bits == rhs.bits; }
    [[nodiscard]] auto operator!=(const Value &rhs) const noexcept -> bool { return bits != rhs.bits; }
    [[nodiscard]] auto identity_key() const noexcept -> uint64_t { return bits; }
    void reset() noexcept { *this = Value(); }

    // High-level helpers
    [[nodiscard]] auto type_name() const -> std::string;
    auto to_repr(State &S) const -> std::string;

    // Setters
    void set_number(double value) noexcept;
    void set_pair(PairData *ptr) noexcept;
    void set_string(StringData *ptr) noexcept;
    void set_symbol(StringData *ptr) noexcept;
    void set_func(FuncData *ptr) noexcept;
    void set_macro(MacroData *ptr) noexcept;
    void set_prim(Prim fn) noexcept;
    void set_cfunc(CFunc fn) noexcept;

  private:
    void retain() const noexcept;
    void release() noexcept;
    [[nodiscard]] auto payload_ptr() const noexcept -> void * { return reinterpret_cast<void *>(bits & kPayloadMask); }
    static void retain_payload(Type t, void *p) noexcept;
    static void release_payload(Type t, void *p) noexcept;
    static auto is_refcounted(Type t) noexcept -> bool;

    // Template helpers declarations (member templates so definitions can
    // access private members like `bits` and `release`).
    template <uint64_t Tag, typename DataT>
    inline auto get_payload_raw() const noexcept -> DataT *;

    template <uint64_t Tag, typename DataT>
    inline void set_payload_raw(DataT *ptr) noexcept;

    template <uint64_t Tag, typename Fn>
    inline auto get_fn_raw() const noexcept -> Fn;

    template <uint64_t Tag, typename Fn>
    inline void set_fn_raw(Fn fn) noexcept;

    // Use NaN-boxing to store all value types in a single 64-bit integer
    // Runtime assumptions are documented in the .cpp implementation.
    uint64_t bits;
};

// Inline short Value methods for performance
inline auto Value::get_number() const noexcept -> double {
    double result;
    static_assert(sizeof(double) == sizeof(bits), "Double must be 64-bit");
    std::memcpy(&result, &bits, sizeof(result));
    return result;
}

inline void Value::set_number(double value) noexcept {
    release();
    std::memcpy(&bits, &value, sizeof(bits));
    if ((bits & kNaNMask) == kNaNMask)
        bits = 0;
}

// Member template definitions (declared above in the private section).
template <uint64_t Tag, typename DataT>
inline __attribute__((always_inline)) auto Value::get_payload_raw() const noexcept -> DataT * { return reinterpret_cast<DataT *>(bits & kPayloadMask); }

template <uint64_t Tag, typename DataT>
inline __attribute__((always_inline)) void Value::set_payload_raw(DataT *ptr) noexcept {
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
inline void Value::set_fn_raw(Fn fn) noexcept {
    release();
    uint64_t payload = 0;
    std::memcpy(&payload, &fn, sizeof(fn));
    bits = Tag | (payload & kPayloadMask);
}

inline __attribute__((always_inline)) auto Value::get_pair() const noexcept -> PairData * { return get_payload_raw<kTagPair, PairData>(); }
inline void Value::set_pair(PairData *ptr) noexcept { set_payload_raw<kTagPair, PairData>(ptr); }

inline __attribute__((always_inline)) auto Value::get_string() const noexcept -> std::string * {
    auto *sd = get_payload_raw<kTagString, StringData>();
    return sd ? &sd->value : nullptr;
}
inline void Value::set_string(StringData *ptr) noexcept { set_payload_raw<kTagString, StringData>(ptr); }

inline __attribute__((always_inline)) auto Value::get_symbol() const noexcept -> std::string * {
    auto *sd = get_payload_raw<kTagSymbol, StringData>();
    return sd ? &sd->value : nullptr;
}
inline void Value::set_symbol(StringData *ptr) noexcept { set_payload_raw<kTagSymbol, StringData>(ptr); }

inline auto Value::get_func() const noexcept -> FuncData * { return get_payload_raw<kTagFunc, FuncData>(); }
inline void Value::set_func(FuncData *ptr) noexcept { set_payload_raw<kTagFunc, FuncData>(ptr); }

inline auto Value::get_macro() const noexcept -> MacroData * { return get_payload_raw<kTagMacro, MacroData>(); }
inline void Value::set_macro(MacroData *ptr) noexcept { set_payload_raw<kTagMacro, MacroData>(ptr); }

inline Prim Value::get_prim() const noexcept { return get_fn_raw<kTagPrim, Prim>(); }
inline void Value::set_prim(Prim fn) noexcept { set_fn_raw<kTagPrim, Prim>(fn); }

inline CFunc Value::get_cfunc() const noexcept { return get_fn_raw<kTagCFunc, CFunc>(); }
inline void Value::set_cfunc(CFunc fn) noexcept { set_fn_raw<kTagCFunc, CFunc>(fn); }

inline __attribute__((always_inline)) void Value::retain() const noexcept {
    Type t = get_type();
    if (!is_refcounted(t))
        return;
    retain_payload(t, payload_ptr());
}

inline __attribute__((always_inline)) void Value::release() noexcept {
    Type t = get_type();
    if (!is_refcounted(t))
        return;
    release_payload(t, payload_ptr());
    bits = kTagNil;
}

inline auto Value::is_refcounted(Type t) noexcept -> bool {
    // Use a constexpr lookup table indexed by the Type enum value for
    // faster, branch-free checks and better inlining opportunities.
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
    Value car;
    Value cdr;
};

// Function and macro runtime representations used by the evaluator.
//
// FuncData fields:
// - params, body: AST nodes pointing to parameter list and function body
// - closure_env: captured lexical environment
// - call_count: a simple call counter used to decide when to JIT compile
// - num_call_count: counter for pure numeric calls
// - compiled_code: a void* that holds the machine-code pointer returned by
//                  the JITCompiler after successful compilation (nullptr if not compiled)
class FuncData : public RcBase {
  public:
    Value params;
    Value body;
    Env *closure_env = nullptr;
    size_t call_count = 0;
    size_t num_call_count = 0;
    void *compiled_code = nullptr;
    bool jit_failed = false;
};

// MacroData: macros are expanded by the interpreter at compile-time (no JIT)
class MacroData : public RcBase {
  public:
    Value params;
    Value body;
    Env *closure_env = nullptr;
};

} // namespace vdlisp

#endif // VDLISP__NANBOX_HPP