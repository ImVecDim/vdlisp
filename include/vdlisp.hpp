#pragma once
#ifndef VDLISP__VDLISP_HPP
#define VDLISP__VDLISP_HPP

// ============================================================================
// vdlisp — Public API Header
// ============================================================================
//
// A compact Lisp interpreter written in C++23.  This header exposes the
// complete public interface:
//
//   Value   — NaN-boxed universal value (nil, number, string, symbol, pair,
//             function, macro, primitive, C function)
//   State   — interpreter global state (parser, evaluator, symbol table,
//             allocators, source-location tracking)
//   Env     — lexical environment (bindings map + parent chain)
//
// External consumers (REPL, embedding hosts) need only this header.
// Internal implementation details (SlabPool, ListBuilder, etc.) live in
// src/state.hpp.
// ============================================================================

#include <array>
#include <bit>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <boost/container/small_vector.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <boost/unordered/unordered_flat_map.hpp>

// ============================================================================
// DLL Export / Import Macro
// ============================================================================
// On Windows, dllexport is sufficient for both static and dynamic linking
// when using MinGW — dllimport is not required for correct resolution.

#ifdef _WIN32
  #define VDLISP_API __declspec(dllexport)
#else
  #define VDLISP_API __attribute__((visibility("default")))
#endif

namespace vdlisp {

// ============================================================================
// Forward Declarations
// ============================================================================

class Value;
class PairData;
class StringData;
class FuncData;
class MacroData;
class State;
class Env;

// ============================================================================
// Function Pointer Type Aliases
// ============================================================================

/// A primitive (special form) receives unevaluated arguments and the
/// current lexical environment.  It controls its own evaluation strategy.
using Prim  = Value (*)(State &, const Value &, Env *);

/// A C-callable built-in function receives already-evaluated arguments
/// (applicative order).  No environment is passed because arguments are
/// evaluated before the call.
using CFunc = Value (*)(State &, const Value &);

// ============================================================================
// Type Tag Enumeration
// ============================================================================

/// Discriminates the nine kinds of values representable by NaN-boxing.
/// The numeric values correspond to the tag bits stored in bits [51:48]
/// of the 64-bit encoding.
enum Type : uint8_t {
    TNIL    = 0,  ///< the empty list / false
    TPAIR   = 1,  ///< cons cell (car . cdr)
    TNUMBER = 2,  ///< IEEE 754 double (stored directly in the word)
    TSTRING = 3,  ///< heap-allocated string (StringData)
    TSYMBOL = 4,  ///< interned symbol (StringData)
    TFUNC   = 5,  ///< user-defined closure (FuncData)
    TMACRO  = 6,  ///< user-defined macro (MacroData)
    TPRIM   = 7,  ///< primitive / special form (Prim function pointer)
    TCFUNC  = 8,  ///< C built-in function (CFunc function pointer)
};

// ============================================================================
// Source Location
// ============================================================================

/// Attaches file/line/column information to an AST node or error.
/// An optional `label` can annotate the context (e.g. "macro", "fn").
struct SourceLoc {
    std::string file;
    size_t      line  = 0;
    size_t      col   = 0;
    std::string label;
};

// ============================================================================
// Reference-counting Base
// ============================================================================

/// Intrusive reference-counting base class.  Objects start with ref count 1.
/// Copy and move are deleted — lifetime is managed exclusively through
/// retain / release calls (or intrusive_ptr for Env).
struct RcBase {
    size_t refs_{1};

    RcBase()                          noexcept = default;
    ~RcBase()                         noexcept = default;
    RcBase(const RcBase &)            = delete;
    RcBase &operator=(const RcBase &) = delete;

    /// Atomically increment the reference count.
    [[gnu::always_inline]] inline void   inc_ref() noexcept { ++refs_; }

    /// Atomically decrement the reference count.
    /// @return the new count (caller must check for zero before destroying).
    [[gnu::always_inline]] inline size_t dec_ref() noexcept { return --refs_; }
};

// ============================================================================
// StringData — Heap-allocated String Backing
// ============================================================================

/// Owns a std::string payload.  StringData objects are allocated from a
/// SlabPool; `operator delete` is intentionally a no-op so the slab block
/// is freed in one batch at shutdown.
class StringData : public RcBase {
  public:
    StringData() noexcept = default;
    explicit StringData(std::string_view s) : value(s) {}

    std::string value;

    /// No-op: memory is reclaimed by SlabPool::purge().
    static void operator delete(void *p) noexcept {}
};

// ============================================================================
// Transparent Hashers for Heterogeneous Lookup
// ============================================================================

/// Transparent hash functor: allows std::string_view to be used directly
/// when looking up keys in boost::unordered_flat_map<std::string, T>,
/// avoiding a temporary std::string allocation.
struct StringHash {
    using is_transparent = void;

    auto operator()(std::string_view sv) const noexcept { return std::hash<std::string_view>{}(sv); }
    auto operator()(const std::string &s) const noexcept { return std::hash<std::string>{}(s);     }
};

/// Transparent equality functor: supports comparisons between
/// std::string_view and std::string in any order.
struct StringEqual {
    using is_transparent = void;

    constexpr auto operator()(std::string_view a,     const std::string &b) const noexcept { return a == b; }
    constexpr auto operator()(const std::string &a, std::string_view b)      const noexcept { return a == b; }
    constexpr auto operator()(const std::string &a, const std::string &b)    const noexcept { return a == b; }
};

// ============================================================================
// intrusive_ptr ADL Hooks (Forward Declarations)
// ============================================================================
// Must appear before the Env class body so that boost::intrusive_ptr<Env>
// can be used as a member of Env itself (parent chain).  Two-phase lookup
// would otherwise fail.

inline void intrusive_ptr_add_ref(Env *e) noexcept;
inline void intrusive_ptr_release(Env *e) noexcept;

// ============================================================================
// Env — Lexical Environment
// ============================================================================

/// A lexical scope: a flat map of symbol-name → Value bindings plus an
/// optional parent environment.  Environments form a singly-linked chain
/// searched outward by `State::lookup`.
class VDLISP_API Env : public RcBase {
  public:
    boost::unordered_flat_map<std::string, Value> map;    ///< bindings in this scope
    boost::intrusive_ptr<Env>                    parent;  ///< enclosing scope (may be null)
};

// ============================================================================
// Env Reference Counting Implementation
// ============================================================================

/// Boost intrusive_ptr contract: called on add_ref, no null check required.
[[gnu::always_inline]] inline void intrusive_ptr_add_ref(Env *e) noexcept {
    e->inc_ref();
}

/// Boost intrusive_ptr contract: called on release, no null check.
/// Triggers deletion when the reference count drops to zero.
[[gnu::always_inline]] inline void intrusive_ptr_release(Env *e) noexcept {
    if (e->dec_ref() == 0) delete e;
}

// ============================================================================
// EnvGuard — RAII Wrapper for a Temporary Environment Reference
// ============================================================================
// Typical usage: create a new Env, wrap it in EnvGuard, and it is
// automatically released when the guard goes out of scope.

struct EnvGuard {
    /// Takes ownership of an environment reference (may be nullptr).
    explicit EnvGuard(Env *e = nullptr) noexcept : e_(e) {}

    /// Releases the held environment reference.
    ~EnvGuard() { if (e_) intrusive_ptr_release(e_); }

    EnvGuard(const EnvGuard &)            = delete;
    EnvGuard &operator=(const EnvGuard &) = delete;

    /// Move constructor: transfers ownership, leaving source empty.
    EnvGuard(EnvGuard &&o) noexcept : e_(std::exchange(o.e_, nullptr)) {}

    /// Move assignment: releases current, takes ownership from source.
    EnvGuard &operator=(EnvGuard &&o) noexcept {
        if (this != &o) {
            if (e_) intrusive_ptr_release(e_);
            e_ = std::exchange(o.e_, nullptr);
        }
        return *this;
    }

  private:
    Env *e_;  ///< owned environment pointer (may be nullptr)
};

// ============================================================================
// Value — NaN-Boxed Universal Value
// ============================================================================
//
// A single 64-bit word encodes one of nine Lisp value types.
//
//   Number (IEEE 754 double):
//     The raw bits of the double are stored directly.  A valid double whose
//     exponent bits are NOT all 1 is interpreted as a number.  Numbers with
//     all exponent bits set (NaN / Infinity) conflict with the boxing tag
//     and are rejected (see set_number).
//
//   Boxed types (pair, string, symbol, func, macro, prim, cfunc):
//     The top 16 bits contain the type tag and NaN exponent mask.
//     The lower 48 bits store a heap pointer (for ref-counted types)
//     or a function pointer (for prim / cfunc).
//
//   Nil:
//     Stored as a boxed value with payload 0 (kTagNil).
//
// Bit layout (64 bits):
//   [63:52]  exponent  — must be 0x7FF for boxed, anything else for double
//   [51:48]  tag       — Type enum value for boxed
//   [47:0]   payload   — heap / function pointer (48-bit canonical addr)

class VDLISP_API Value {
  public:
    // ---- NaN-boxing bit masks ----

    /// Bits that must be set to identify a QNaN boxed value.
    static constexpr uint64_t kNaNMask     = 0x7FF0000000000000ULL;

    /// Combined tag + NaN mask for efficient identity check.
    static constexpr uint64_t kTagMask     = kNaNMask | 0x000F000000000000ULL;

    /// Low 48 bits: pointer or function-pointer payload.
    static constexpr uint64_t kPayloadMask = 0x0000FFFFFFFFFFFFULL;

    // ---- Per-type tag constants (top 16 bits of the encoding) ----

    static constexpr uint64_t kTagNil    = kNaNMask | 0x0000000000000000ULL;
    static constexpr uint64_t kTagPair   = kNaNMask | 0x0001000000000000ULL;
    static constexpr uint64_t kTagString = kNaNMask | 0x0002000000000000ULL;
    static constexpr uint64_t kTagSymbol = kNaNMask | 0x0003000000000000ULL;
    static constexpr uint64_t kTagFunc   = kNaNMask | 0x0004000000000000ULL;
    static constexpr uint64_t kTagMacro  = kNaNMask | 0x0005000000000000ULL;
    static constexpr uint64_t kTagPrim   = kNaNMask | 0x0006000000000000ULL;
    static constexpr uint64_t kTagCFunc  = kNaNMask | 0x0007000000000000ULL;

    // ---- Constructors ----

    /// Default: constructs nil.
    Value() noexcept : bits(kTagNil) {}

    /// Construct a boxed value with the given type tag (payload initially 0).
    explicit Value(Type t) noexcept;

    /// Construct nil from nullptr (convenience for `if (v) ...` idioms).
    Value(std::nullptr_t) noexcept : bits(kTagNil) {}

    /// Copy constructor: increments the reference count of boxed payloads.
    Value(const Value &other) noexcept;

    /// Move constructor: transfers ownership, leaves source as nil.
    Value(Value &&other) noexcept;

    /// Destructor: releases any owned reference-counted payload.
    ~Value() noexcept;

    // ---- Assignment operators ----

    auto operator=(const Value &other)  noexcept -> Value &;
    auto operator=(Value &&other)       noexcept -> Value &;
    auto operator=(std::nullptr_t)      noexcept -> Value &;

    // ========================================================================
    // Getters
    // ========================================================================

    /// Returns the type tag.  The common case (number) is predicted unlikely
    /// to encourage the compiler to layout the boxed-type path favorably.
    [[nodiscard]] inline auto get_type() const noexcept -> Type {
        // If the exponent bits are not all 1s, this is a plain double.
        if ((bits & kNaNMask) != kNaNMask) [[unlikely]]
            return TNUMBER;
        // Look up the 4-bit tag in a static table (16 entries for safety).
        static constexpr std::array kTagMap = {
            TNIL, TPAIR, TSTRING, TSYMBOL,
            TFUNC, TMACRO, TPRIM, TCFUNC,
            TNIL, TNIL, TNIL, TNIL,
            TNIL, TNIL, TNIL, TNIL};
        return kTagMap[(bits >> 48) & 0xF];
    }

    /// Extract the raw double value.  Caller must ensure the type is TNUMBER.
    [[nodiscard]] auto get_number() const noexcept -> double;

    /// Access the PairData payload.  Caller must ensure the type is TPAIR.
    [[nodiscard]] auto get_pair()   const noexcept -> PairData *;

    /// Access the string payload.  May return nullptr for a nil string.
    [[nodiscard]] auto get_string()  const noexcept -> std::string *;

    /// Access the symbol name.  May return nullptr for an uninterned symbol.
    [[nodiscard]] auto get_symbol()  const noexcept -> std::string *;

    /// Access the FuncData payload.  Caller must ensure the type is TFUNC.
    [[nodiscard]] auto get_func()    const noexcept -> FuncData *;

    /// Access the MacroData payload.  Caller must ensure the type is TMACRO.
    [[nodiscard]] auto get_macro()   const noexcept -> MacroData *;

    /// Recover the Prim function pointer from a TPRIM value.
    [[nodiscard]] auto get_prim()    const noexcept -> Prim;

    /// Recover the CFunc function pointer from a TCFUNC value.
    [[nodiscard]] auto get_cfunc()   const noexcept -> CFunc;

    // ---- Boolean / nullptr semantics ----

    /// Evaluates to true for any non-nil value.
    [[nodiscard]] explicit operator bool() const noexcept { return get_type() != TNIL; }

    /// nil == nullptr.
    [[nodiscard]] auto operator==(std::nullptr_t) const noexcept -> bool { return get_type() == TNIL; }

    /// non-nil != nullptr.
    [[nodiscard]] auto operator!=(std::nullptr_t) const noexcept -> bool { return get_type() != TNIL; }

    /// Identity equality: two Values are equal iff their bit patterns match.
    [[nodiscard]] auto operator==(const Value &rhs) const noexcept -> bool { return bits == rhs.bits; }
    [[nodiscard]] auto operator!=(const Value &rhs) const noexcept -> bool { return bits != rhs.bits; }

    /// Returns the raw 64-bit encoding, usable as a unique key for maps.
    [[nodiscard]] auto identity_key() const noexcept -> uint64_t { return bits; }

    /// Resets this value to nil, releasing any owned payload.
    auto reset() noexcept -> void { *this = Value(); }

    /// Human-readable type name (e.g. "number", "pair", "function").
    [[nodiscard]] auto type_name() const noexcept -> std::string_view;

    /// Append a Lisp-readable representation to `out`.
    auto to_repr(State &S, std::string &out) const -> void;

    // ========================================================================
    // Setters
    // ========================================================================

    /// Store a double.  Throws LispError if `value` is NaN (the NaN exponent
    /// bits would conflict with NaN-boxing tags).
    auto set_number(double value)       -> void;

    /// Set the PairData payload (TPAIR tag).
    auto set_pair(PairData *ptr)        noexcept -> void;

    /// Set the StringData payload (TSTRING tag).
    auto set_string(StringData *ptr)    noexcept -> void;

    /// Set the symbol payload (TSYMBOL tag).
    auto set_symbol(StringData *ptr)    noexcept -> void;

    /// Set the FuncData payload (TFUNC tag).
    auto set_func(FuncData *ptr)        noexcept -> void;

    /// Set the MacroData payload (TMACRO tag).
    auto set_macro(MacroData *ptr)      noexcept -> void;

    /// Set the Prim function pointer payload (TPRIM tag).
    auto set_prim(Prim fn)              noexcept -> void;

    /// Set the CFunc function pointer payload (TCFUNC tag).
    auto set_cfunc(CFunc fn)            noexcept -> void;

  private:
    // ---- Reference-counting helpers ----

    /// Increment the reference count of the current boxed payload (if any).
    auto retain() const noexcept -> void;

    /// Decrement and possibly destroy the current boxed payload.
    auto release() noexcept -> void;

    /// Extract the raw 48-bit payload pointer as void*.
    [[nodiscard]] auto payload_ptr() const noexcept -> void * {
        return reinterpret_cast<void *>(bits & kPayloadMask);
    }

    /// Retain a generic payload (used by retain() dispatch).
    static auto retain_payload(Type t, void *p) noexcept -> void;

    /// Release a generic payload (handles dec_ref and destruction).
    static auto release_payload(Type t, void *p) noexcept -> void;

    /// Returns true for types that carry a reference-counted payload.
    static auto is_refcounted(Type t) noexcept -> bool;

    // ---- Generic (de)serialization templates ----

    /// Extract a payload pointer for a specific Tag → DataT pair.
    template <uint64_t Tag, typename DataT>
    inline auto get_payload_raw() const noexcept -> DataT *;

    /// Encode a payload pointer with a specific Tag.
    template <uint64_t Tag, typename DataT>
    inline void set_payload_raw(DataT *ptr) noexcept;

    /// Extract a function pointer for a specific Tag → Fn pair.
    template <uint64_t Tag, typename Fn>
    inline auto get_fn_raw() const noexcept -> Fn;

    /// Encode a function pointer with a specific Tag.
    template <uint64_t Tag, typename Fn>
    inline void set_fn_raw(Fn fn) noexcept;

    // ---- Storage ----

    uint64_t bits;  ///< NaN-boxed 64-bit encoding
};

// ============================================================================
// Value — Inline Implementations
// ============================================================================

inline auto Value::get_number() const noexcept -> double {
    return std::bit_cast<double>(bits);
}

// ============================================================================
// Generic Payload Access
// ============================================================================

template <uint64_t Tag, typename DataT>
[[gnu::always_inline]] inline auto Value::get_payload_raw() const noexcept -> DataT * {
    return reinterpret_cast<DataT *>(bits & kPayloadMask);
}

template <uint64_t Tag, typename DataT>
inline void Value::set_payload_raw(DataT *ptr) noexcept {
    uint64_t newp = reinterpret_cast<uint64_t>(ptr) & kPayloadMask;
    // Short-circuit if the tag and payload are unchanged (avoids refcount churn).
    if (((bits & kTagMask) == Tag) && ((bits & kPayloadMask) == newp))
        return;
    release();
    bits = Tag | newp;
}

template <uint64_t Tag, typename Fn>
inline auto Value::get_fn_raw() const noexcept -> Fn {
    uint64_t payload = bits & kPayloadMask;
    return std::bit_cast<Fn>(payload);
}

template <uint64_t Tag, typename Fn>
inline auto Value::set_fn_raw(Fn fn) noexcept -> void {
    release();
    uint64_t payload = std::bit_cast<uint64_t>(fn);
    bits = Tag | (payload & kPayloadMask);
}

// ============================================================================
// Typed Getter / Setter Wrappers
// ============================================================================

[[gnu::always_inline]] inline auto Value::get_pair()   const noexcept -> PairData *  { return get_payload_raw<kTagPair,   PairData>();   }
                         inline auto Value::set_pair(PairData *ptr) noexcept -> void  { set_payload_raw<kTagPair,   PairData>(ptr);      }

[[gnu::always_inline]] inline auto Value::get_string() const noexcept -> std::string * {
    auto *sd = get_payload_raw<kTagString, StringData>();
    return sd ? &sd->value : nullptr;
}
inline auto Value::set_string(StringData *ptr) noexcept -> void { set_payload_raw<kTagString, StringData>(ptr); }

[[gnu::always_inline]] inline auto Value::get_symbol() const noexcept -> std::string * {
    auto *sd = get_payload_raw<kTagSymbol, StringData>();
    return sd ? &sd->value : nullptr;
}
inline auto Value::set_symbol(StringData *ptr) noexcept -> void { set_payload_raw<kTagSymbol, StringData>(ptr); }

inline auto Value::get_func()  const noexcept -> FuncData *  { return get_payload_raw<kTagFunc,  FuncData>();  }
inline auto Value::set_func(FuncData *ptr) noexcept -> void  { set_payload_raw<kTagFunc,  FuncData>(ptr);     }
inline auto Value::get_macro() const noexcept -> MacroData * { return get_payload_raw<kTagMacro, MacroData>(); }
inline auto Value::set_macro(MacroData *ptr) noexcept -> void{ set_payload_raw<kTagMacro, MacroData>(ptr);    }

inline auto Value::get_prim()  const noexcept -> Prim  { return get_fn_raw<kTagPrim,  Prim>();  }
inline auto Value::set_prim(Prim fn) noexcept -> void  { set_fn_raw<kTagPrim,  Prim>(fn);        }
inline auto Value::get_cfunc() const noexcept -> CFunc { return get_fn_raw<kTagCFunc, CFunc>(); }
inline auto Value::set_cfunc(CFunc fn) noexcept -> void{ set_fn_raw<kTagCFunc, CFunc>(fn);       }

// ============================================================================
// Reference-counting Dispatching
// ============================================================================

[[gnu::always_inline]] inline auto Value::retain() const noexcept -> void {
    Type t = get_type();
    if (!is_refcounted(t)) return;
    retain_payload(t, payload_ptr());
}

[[gnu::always_inline]] inline auto Value::release() noexcept -> void {
    Type t = get_type();
    if (!is_refcounted(t)) return;
    release_payload(t, payload_ptr());
    bits = kTagNil;
}

inline auto Value::is_refcounted(Type t) noexcept -> bool {
    static constexpr std::array kIsRefcounted = {
        false,  // TNIL
        true,   // TPAIR
        false,  // TNUMBER
        true,   // TSTRING
        true,   // TSYMBOL
        true,   // TFUNC
        true,   // TMACRO
        false,  // TPRIM
        false,  // TCFUNC
    };
    return static_cast<size_t>(t) < std::size(kIsRefcounted)
        && kIsRefcounted[static_cast<size_t>(t)];
}

/// Increments the reference count of a boxed payload.  Safe to call with
/// a null pointer (nil payload).
[[gnu::always_inline]] inline void Value::retain_payload(Type /*t*/, void *p) noexcept {
    if (p) static_cast<RcBase *>(p)->inc_ref();
}

// ============================================================================
// PairData — Cons Cell Backing
// ============================================================================
// operator delete is a no-op; PairData objects are allocated from a SlabPool.
// When the reference count reaches zero, the destructor is invoked to
// release child Values, but the memory is reclaimed in batch at shutdown.

class PairData : public RcBase {
  public:
    Value car;   ///< first element
    Value cdr;   ///< rest of the list (or second element in a dotted pair)
    static void operator delete(void *p) noexcept {}
};

// ============================================================================
// ClosureData<D> — Base for FuncData / MacroData
// ============================================================================
// The template parameter D exists solely to give FuncData and MacroData
// distinct types so that SlabPool<FuncData> and SlabPool<MacroData> are
// separate allocators.  The destructor releases the closure environment
// reference.

template <typename>
struct ClosureData : RcBase {
    Value params;                    ///< formal parameter list
    Value body;                      ///< expression(s) to evaluate
    Env  *closure_env = nullptr;     ///< captured lexical environment (null = top-level)

    ~ClosureData() noexcept {
        if (closure_env) { intrusive_ptr_release(closure_env); closure_env = nullptr; }
    }
    static void operator delete(void *p) noexcept {}
};

struct FuncData  : ClosureData<FuncData>  {};  ///< user-defined function closure
struct MacroData : ClosureData<MacroData> {};  ///< user-defined macro closure

// ============================================================================
// LispError — Exception with Source Location and Call Chain
// ============================================================================

/// Thrown when a runtime error occurs.  Carries optional source location
/// and a chain of call frames for diagnostics.
struct VDLISP_API LispError : public std::runtime_error {
    SourceLoc  loc;       ///< primary error location (if known)
    using Chain = boost::container::small_vector<SourceLoc, 4>;
    Chain      call_chain;///< stack of call frames leading to the error
    bool       has_loc = false;  ///< true if `loc` is valid

    /// Error without source location.
    explicit LispError(const std::string &msg)
        : std::runtime_error(msg), has_loc(false) {}

    /// Error with a single source location.
    LispError(SourceLoc loc, const std::string &msg)
        : std::runtime_error(msg), loc(std::move(loc)), has_loc(true) {}

    /// Error with source location and full call chain.
    LispError(SourceLoc loc, const std::string &msg, Chain chain)
        : std::runtime_error(msg), loc(std::move(loc)),
          call_chain(std::move(chain)), has_loc(true) {}
};

// ============================================================================
// Value::set_number — Defined After LispError (for throw)
// ============================================================================

inline auto Value::set_number(double value) -> void {
    uint64_t raw = std::bit_cast<uint64_t>(value);
    // NaN / Infinity have all exponent bits set (0x7FF), which is
    // indistinguishable from the NaN-boxing tag mask.  Reject them.
    if ((raw & kNaNMask) == kNaNMask)
        throw LispError("cannot store NaN or Infinity as a Value "
                        "(conflicts with NaN-boxing tag)");
    release();
    bits = raw;
}

// ============================================================================
// Public Free Functions
// ============================================================================

/// Print an error message with source-location context (file:line:col)
/// and a caret pointing to the error column.
VDLISP_API auto print_error_with_loc(const State &S, const SourceLoc &loc,
                                     const std::string &msg) -> void;

/// Structural (deep) equality comparison for Lisp values.
/// Two values are equal if they have the same type and structure,
/// comparing pair elements recursively.
[[nodiscard]] auto value_equal(const Value &a, const Value &b) -> bool;

// ============================================================================
// Pair Access Helpers
// ============================================================================

/// Generic pair field access through pointer-to-member.
/// Returns a reference to a static nil Value if `p` is not a pair.
template <auto Member>
[[nodiscard]] [[gnu::always_inline]] inline const Value &
pair_access(const Value &p) noexcept {
    if (!p || p.get_type() != TPAIR) {
        static const Value kNil;
        return kNil;
    }
    return (p.get_pair()->*Member);
}

/// Get the car (first element) of a pair, or nil if not a pair.
[[nodiscard]] [[gnu::always_inline]] inline const Value &
pair_car(const Value &p) noexcept { return pair_access<&PairData::car>(p); }

/// Get the cdr (rest) of a pair, or nil if not a pair.
[[nodiscard]] [[gnu::always_inline]] inline const Value &
pair_cdr(const Value &p) noexcept { return pair_access<&PairData::cdr>(p); }

/// Returns true iff `p` is a non-nil pair.
[[nodiscard]] [[gnu::always_inline]] inline auto
is_pair(const Value &p) noexcept -> bool {
    return p && p.get_type() == TPAIR;
}

/// Returns true iff `p` is a symbol with the given name.
[[nodiscard]] [[gnu::always_inline]] inline auto
is_symbol(const Value &p, const std::string &name) -> bool {
    return p && p.get_type() == TSYMBOL && *p.get_symbol() == name;
}

/// Generic pair field mutation through pointer-to-member.
/// Silently ignores non-pair values.
template <auto Member>
[[gnu::always_inline]] inline void
pair_set(const Value &p, const Value &v) noexcept {
    if (!p || p.get_type() != TPAIR) return;
    p.get_pair()->*Member = v;
}

/// Set the car of a pair.  No-op if `p` is not a pair.
[[gnu::always_inline]] inline void
pair_set_car(const Value &p, const Value &v) noexcept { pair_set<&PairData::car>(p, v); }

/// Set the cdr of a pair.  No-op if `p` is not a pair.
[[gnu::always_inline]] inline void
pair_set_cdr(const Value &p, const Value &v) noexcept { pair_set<&PairData::cdr>(p, v); }

// ============================================================================
// List Iteration
// ============================================================================

/// Iterate over a Lisp list, invoking `F(car)` for each element.
/// Stops when the list ends (nil or non-pair).
inline void foreach_lisp(const Value &list, auto &&F) {
    const Value *cur = &list;
    while (*cur && cur->get_type() == TPAIR) {
        PairData *pd = cur->get_pair();
        F(pd->car);
        cur = &pd->cdr;
    }
}

// ============================================================================
// Closure Environment Management
// ============================================================================

/// Break a function or macro's link to its closure environment (set to
/// nullptr) without decrementing the environment's reference count.
/// Used during shutdown to break reference cycles before pool purging.
VDLISP_API void clear_closure_env(Value &v) noexcept;

// ============================================================================
// State — Interpreter Global State
// ============================================================================
//
// State owns the global environment, the symbol intern table, the slab
// allocators, and source-location metadata.  It provides the complete
// interpreter API: parsing, evaluation, function/macro creation, and
// environment manipulation.
//
// All public fields are accessed through accessor methods to encapsulate
// internal data structures.

class VDLISP_API State {
public:
  /// Construct a new interpreter state.  Registers all built-in functions
  /// and primitives, creates the global environment, and interns `#t`.
  explicit State();

  /// Destructor: shuts down pools and releases all resources.
  ~State();

  /// Release all pooled memory and clear internal state.  Safe to call
  /// multiple times (subsequent calls are no-ops).
  auto shutdown_and_purge_pools() -> void;

  // ========================================================================
  // State Query Accessors
  // ========================================================================

  /// Returns a pointer to the global (top-level) environment.
  [[nodiscard]] auto global_env() noexcept -> Env * { return global; }

  /// Returns the expression currently being evaluated (for error reporting).
  [[nodiscard]] auto current_expression() const noexcept -> const Value & {
    return current_expr;
  }

  /// Looks up a call-chain entry by expression identity key.
  /// Returns nullptr if no entry exists.
  [[nodiscard]] auto call_chain_for(uint64_t key) const noexcept
      -> const LispError::Chain * {
    auto it = src_call_chain_map.find(key);
    return it != src_call_chain_map.end() ? &it->second : nullptr;
  }

  /// Checks whether a module has already been loaded.
  /// Returns a pointer to the cached result, or nullptr if not loaded.
  [[nodiscard]] auto module_loaded(const std::string &key) const noexcept
      -> const Value * {
    auto it = loaded_modules.find(key);
    return it != loaded_modules.end() ? &it->second : nullptr;
  }

  /// Caches a module load result under the given canonical key.
  auto set_module(const std::string &key, Value v) -> void {
    loaded_modules[key] = std::move(v);
  }

  // ========================================================================
  // Factory Methods — Create Lisp Values
  // ========================================================================

  /// Create nil (the empty list).
  [[nodiscard]] auto make_nil() noexcept -> Value { return {}; }

  /// Create a number Value from a double.  May throw if `n` is NaN or
  /// Infinity (conflicts with NaN-boxing tag bits).
  [[nodiscard]] auto make_number(double n) -> Value;

  /// Create a string Value (allocates StringData from the string pool).
  [[nodiscard]] auto make_string(const std::string &s) -> Value;

  /// Create a symbol Value.  Symbols are interned: the same name always
  /// returns the same identity (pointer equality).
  [[nodiscard]] auto make_symbol(std::string_view s) -> Value;

  /// Create a cons cell (car . cdr).
  [[nodiscard]] auto make_pair(Value car, Value cdr) -> Value;

  /// Create a C-callable built-in function Value.
  [[nodiscard]] auto make_cfunc(const CFunc &fn) noexcept -> Value;

  /// Create a user-defined function closure, capturing the given environment.
  [[nodiscard]] auto make_function(Value params, Value body, Env *env) -> Value;

  /// Create a primitive (special form) Value.
  [[nodiscard]] auto make_prim(const Prim &fn) noexcept -> Value;

  /// Create a user-defined macro closure, capturing the given environment.
  [[nodiscard]] auto make_macro(Value params, Value body, Env *env) -> Value;

  /// Allocate a new lexical environment, optionally linked to a parent.
  /// Returns a raw pointer; use EnvGuard for RAII or intrusive_ptr for
  /// shared ownership.
  [[nodiscard]] auto make_env(Env *parent = nullptr) -> Env *;

  /// Build a Lisp list from an iterator range `[b, e)` of strings.
  template <class It>
  [[nodiscard]] auto make_string_list(It b, It e) -> Value {
      Value head;
      Value *last = &head;
      for (; b != e; ++b) {
          Value sv = make_string(std::string(*b));
          *last = make_pair(std::move(sv), Value());
          PairData *pd = (*last).get_pair();
          last = &pd->cdr;
      }
      return head;
  }

  /// Build a Lisp list from `argc`/`argv`, starting at index `start`.
  /// Convenience wrapper for exposing command-line arguments to Lisp code.
  [[nodiscard]] auto make_string_list(int argc, char **argv, int start = 0) -> Value;

  // ========================================================================
  // Parsing & Evaluation
  // ========================================================================

  /// Parse a single S-expression from `src`.  `name` is used as the
  /// source file identifier in error messages and source-location metadata.
  [[nodiscard]] auto parse(const std::string &src,
                           const std::string &name = "(string)") -> Value;

  /// Parse all top-level expressions from `src`, returning them as a list.
  [[nodiscard]] auto parse_all(const std::string &src,
                               const std::string &name = "(string)") -> Value;

  /// Evaluate an expression in the given lexical environment.
  /// If `env` is nullptr, the global environment is used.
  [[nodiscard]] auto eval(const Value &expr, Env *env) -> Value;

  /// Call a function or primitive with already-evaluated arguments.
  [[nodiscard]] auto call(const Value &fn, const Value &args) -> Value;

  /// Evaluate each expression in `body` sequentially, returning the
  /// result of the last one.
  [[nodiscard]] auto do_list(const Value &body, Env *env) -> Value;

  // ========================================================================
  // Source Location Tracking
  // ========================================================================

  /// Attach a source location to a Value (stored in the identity-keyed map).
  auto set_source_loc(const Value &v, std::string_view file,
                      size_t line, size_t col) -> void;

  /// Retrieve the source location attached to a Value.
  /// Returns false if no location was recorded.
  [[nodiscard]] auto get_source_loc(const Value &v, SourceLoc &out) const -> bool;

  /// Retrieve a single line of source text by file name and line number.
  /// Returns false if the file or line is not available.
  [[nodiscard]] auto get_source_line(std::string_view file, size_t line,
                                     std::string &out) const -> bool;

  // ========================================================================
  // Binding & Environment Utilities
  // ========================================================================

  /// Convert a Value to its Lisp-readable string representation.
  [[nodiscard]] auto to_string(const Value &v) -> std::string;

  /// Register a C-callable built-in function under a global name.
  auto register_builtin(const std::string &name, const CFunc &fn) -> void;

  /// Register a primitive (special form) under a global name.
  auto register_prim(const std::string &name, const Prim &fn) -> void;

  /// Look up a name in an environment chain, returning the bound value
  /// or nil if not found.
  [[nodiscard]] auto get_bound(const std::string &name, Env *env) -> Value {
      if (auto *vp = lookup(name, env))
          return *vp;
      return {};
  }

  /// Look up a name in an environment chain, returning a pointer to the
  /// stored Value (for mutation) or nullptr if not found.
  [[nodiscard]] auto lookup(const std::string &name, Env *env) -> Value *;

  /// Bind a value to a name in the global environment (overwrites existing).
  auto bind_global(const std::string &name, Value v) -> void;

  /// Bind a symbol to a value in the given environment.
  /// If `env` is nullptr, the global environment is used.
  /// @return the stored value (for chaining).
  [[nodiscard]] auto bind(const Value &sym, Value v, Env *env) -> Value;

  /// Mutate an existing binding (set!).  Searches outward through the
  /// environment chain.  If the symbol is not found, creates a new binding
  /// in the current environment.
  /// @return the assigned value.
  [[nodiscard]] auto set(const Value &sym, Value v, Env *env) -> Value;

private:
  // ---- Internal Allocators ----

  [[nodiscard]] auto alloc_string(const std::string &s)      -> StringData *;
  [[nodiscard]] auto alloc_pair(Value &&car, Value &&cdr)    -> PairData *;
  [[nodiscard]] auto alloc_env()                             -> Env *;

  // ---- Friends ----

  friend struct ExprGuard;  ///< RAII guard for current_expr

  // ---- PIMPL: SlabPool Storage ----

  struct PoolData;
  std::unique_ptr<PoolData> pools_;

  // ---- Internal State (exposed through accessor methods) ----

  Env    *global = nullptr;   ///< root lexical environment
  boost::unordered_flat_map<std::string_view, Value> symbol_intern;  ///< symbol interning table
  Value   current_expr;       ///< expression currently under evaluation
  boost::unordered_flat_map<uint64_t, SourceLoc> src_map;            ///< identity-keyed source locations
  boost::unordered_flat_map<uint64_t, LispError::Chain> src_call_chain_map; ///< call-chain metadata
  boost::unordered_flat_map<std::string, std::string, StringHash, StringEqual> sources; ///< source text cache
  boost::unordered_flat_map<std::string, Value> loaded_modules;     ///< module load cache
};

} // namespace vdlisp

#endif // VDLISP__VDLISP_HPP
