#ifndef VDLISP__NANBOX_HPP
#define VDLISP__NANBOX_HPP

#include <cstdint>
#include <cstring>
#include <bit>
#include <string>
#include <unordered_map>


namespace vdlisp
{

  class Value;
  class PairData;
  class StringData;
  class FuncData;
  class MacroData;
  class State;
  class Env;
  //using Value = Value;

  // Use plain function pointer types for primitives and C functions to avoid
  // the overhead of std::function (allocations / type-erasure / indirect calls).
  using Prim = Value (*)(State &, Value, Env*);
  using CFunc = Value (*)(State &, Value);

  enum Type
  {
    TNIL,
    TPAIR,
    TNUMBER,
    TSTRING,
    TSYMBOL,
    TFUNC,  // user function
    TMACRO, // macro
    TPRIM,  // special form (unevaluated args)
    TCFUNC, // c++ builtin
    TENV
  };

  // Forward declarations needed for the implementation
  namespace detail {
    inline auto double_to_bits(double value) -> uint64_t {
      return std::bit_cast<uint64_t>(value);
    }

    inline auto bits_to_double(uint64_t bits) -> double {
      return std::bit_cast<double>(bits);
    }
  } // namespace detail

  struct RcBase {
  protected:
    RcBase(size_t init = 1) noexcept : refs_{init} {}
    ~RcBase() = default;
  private:
    size_t refs_{1};
  public:
    void inc_ref() noexcept { ++refs_; }
    size_t dec_ref() noexcept { return --refs_; }
    size_t ref_count() const noexcept { return refs_; }
  };

  class StringData : public RcBase { public: explicit StringData(const std::string &s) : value(s) {} std::string value; };

  class Env : public RcBase
  {
  public:
    std::unordered_map<std::string, Value> map;
    Env *parent = nullptr;
    ~Env();
  };

  // Helpers to manage Env reference counts (centralized for clarity)
  inline void retain_env(Env *e) { if (e) e->inc_ref(); }
  inline void release_env(Env *e) { if (!e) return; if (e->dec_ref() == 0) delete e; }

  // RAII guard that owns a temporary Env* reference and releases it on destruction.
  struct EnvGuard {
    explicit EnvGuard(Env *e = nullptr) noexcept : e_{e} {}
    ~EnvGuard() { if (e_) release_env(e_); }
    EnvGuard(const EnvGuard&) = delete;
    EnvGuard& operator=(const EnvGuard&) = delete;
    EnvGuard(EnvGuard &&o) noexcept : e_(o.e_) { o.e_ = nullptr; }
    EnvGuard& operator=(EnvGuard &&o) noexcept { if (this != &o) { if (e_) release_env(e_); e_ = o.e_; o.e_ = nullptr; } return *this; }
    Env* get() const noexcept { return e_; }
    Env* release() noexcept { Env* t = e_; e_ = nullptr; return t; }
  private:
    Env *e_;
  };

  class Value
  {
  public:
    // IEEE 754 double format: 1 sign bit + 11 exponent bits + 52 mantissa bits
    // For NaN: exponent is all 1s (0x7FF)
    static constexpr uint64_t kNaNMask     = 0x7FF0000000000000ULL;
    static constexpr uint64_t kTagMask     = kNaNMask | 0x000F000000000000ULL;  // NaN + tag bits
    static constexpr uint64_t kPayloadMask = 0x0000FFFFFFFFFFFFULL; // 48 bits for payload

    // Tags for different pointer types
    static constexpr uint64_t kTagNil     = kNaNMask | 0x0000000000000000ULL;
    static constexpr uint64_t kTagPair    = kNaNMask | 0x0001000000000000ULL;
    static constexpr uint64_t kTagString  = kNaNMask | 0x0002000000000000ULL;
    static constexpr uint64_t kTagSymbol  = kNaNMask | 0x0003000000000000ULL;
    static constexpr uint64_t kTagFunc    = kNaNMask | 0x0004000000000000ULL;
    static constexpr uint64_t kTagMacro   = kNaNMask | 0x0005000000000000ULL;
    static constexpr uint64_t kTagPrim    = kNaNMask | 0x0006000000000000ULL;
    static constexpr uint64_t kTagCFunc   = kNaNMask | 0x0007000000000000ULL;

    Value() : bits(kTagNil) {}
    explicit Value(Type t);
    Value(std::nullptr_t) : bits(kTagNil) {}
    Value(const Value &other);
    Value(Value &&other) noexcept;
    ~Value();

    auto operator=(const Value &other) -> Value&;
    auto operator=(Value &&other) noexcept -> Value&;
    auto operator=(std::nullptr_t) -> Value&;

    // Getters

    // Hot function, inlined for performance. Use branch hint for the common numeric path.
    [[nodiscard]] inline auto get_type() const -> Type {
      // Check if it's a canonical NaN (numbers)
      if (((bits & kNaNMask) != kNaNMask)) [[unlikely]] { // ?????
        return TNUMBER;
      }

      // Check the tag for pointer types
      uint64_t tag = bits & kTagMask;
      switch (tag) {
        case kTagNil:    return TNIL;
        case kTagPair:   return TPAIR;
        case kTagString: return TSTRING;
        case kTagSymbol: return TSYMBOL;
        case kTagFunc:   return TFUNC;
        case kTagMacro:  return TMACRO;
        case kTagPrim:   return TPRIM;
        case kTagCFunc:  return TCFUNC;
        default:         return TNIL;
      }
    }
    [[nodiscard]] auto get_number() const -> double;
    [[nodiscard]] auto get_pair() const -> PairData*;
    [[nodiscard]] auto get_string() const -> std::string*;
    [[nodiscard]] auto get_symbol() const -> std::string*;
    [[nodiscard]] auto get_func() const -> FuncData*;
    [[nodiscard]] auto get_macro() const -> MacroData*;
    [[nodiscard]] Prim get_prim() const;
    [[nodiscard]] CFunc get_cfunc() const;

    [[nodiscard]] inline auto operator->() -> Value* { return this; }
    [[nodiscard]] inline auto operator->() const -> const Value* { return this; }
    [[nodiscard]] inline auto get() -> Value* { return this; }
    [[nodiscard]] inline auto get() const -> const Value* { return this; }
    [[nodiscard]] explicit operator bool() const { return get_type() != TNIL; }
    [[nodiscard]] auto operator==(std::nullptr_t) const -> bool { return get_type() == TNIL; }
    [[nodiscard]] auto operator!=(std::nullptr_t) const -> bool { return get_type() != TNIL; }
    [[nodiscard]] auto operator==(const Value &rhs) const -> bool { return bits == rhs.bits; }
    [[nodiscard]] auto operator!=(const Value &rhs) const -> bool { return bits != rhs.bits; }
    [[nodiscard]] auto identity_key() const -> uint64_t { return bits; }
    void reset() { *this = Value(); }

    // High-level helpers
    [[nodiscard]] auto type_name() const -> std::string;
    auto to_repr(State &S) const -> std::string;

    // Setters
    void set_number(double value);
    void set_pair(PairData* ptr);
    void set_string(StringData* ptr);
    void set_symbol(StringData* ptr);
    void set_func(FuncData* ptr);
    void set_macro(MacroData* ptr);
    void set_prim(Prim fn);
    void set_cfunc(CFunc fn);

  private:
    void retain();
    void release();
    [[nodiscard]] auto payload_ptr() const -> void* { return reinterpret_cast<void*>(bits & kPayloadMask); }
    static void retain_payload(Type t, void* p);
    static void release_payload(Type t, void* p);
    static auto is_refcounted(Type t) -> bool;

    // Use NaN-boxing to store all value types in a single 64-bit integer
    // Runtime assumptions are documented in the .cpp implementation.
    uint64_t bits;
  };

  // Inline short Value methods for performance
  inline auto Value::get_number() const -> double {
    double result;
    static_assert(sizeof(double) == sizeof(bits), "Double must be 64-bit");
    std::memcpy(&result, &bits, sizeof(result));
    return result;
  }

  inline void Value::set_number(double value) {
    release();
    std::memcpy(&bits, &value, sizeof(bits));
    if ((bits & kNaNMask) == kNaNMask) {
      bits = 0;
    }
  }

  inline auto Value::get_pair() const -> PairData* {
    return reinterpret_cast<PairData*>(bits & kPayloadMask);
  }

  inline void Value::set_pair(PairData* ptr) {
    release();
    bits = kTagPair | (reinterpret_cast<uint64_t>(ptr) & kPayloadMask);
  }

  inline auto Value::get_string() const -> std::string* {
    auto *sd = reinterpret_cast<StringData*>(bits & kPayloadMask);
    return sd ? &sd->value : nullptr;
  }

  inline void Value::set_string(StringData* ptr) {
    release();
    bits = kTagString | (reinterpret_cast<uint64_t>(ptr) & kPayloadMask);
  }

  inline auto Value::get_symbol() const -> std::string* {
    auto *sd = reinterpret_cast<StringData*>(bits & kPayloadMask);
    return sd ? &sd->value : nullptr;
  }

  inline void Value::set_symbol(StringData* ptr) {
    release();
    bits = kTagSymbol | (reinterpret_cast<uint64_t>(ptr) & kPayloadMask);
  }

  inline auto Value::get_func() const -> FuncData* {
    return reinterpret_cast<FuncData*>(bits & kPayloadMask);
  }

  inline void Value::set_func(FuncData* ptr) {
    release();
    bits = kTagFunc | (reinterpret_cast<uint64_t>(ptr) & kPayloadMask);
  }

  inline auto Value::get_macro() const -> MacroData* {
    return reinterpret_cast<MacroData*>(bits & kPayloadMask);
  }

  inline void Value::set_macro(MacroData* ptr) {
    release();
    bits = kTagMacro | (reinterpret_cast<uint64_t>(ptr) & kPayloadMask);
  }

  inline Prim Value::get_prim() const {
    Prim fn;
    uint64_t payload = bits & kPayloadMask;
    std::memcpy(&fn, &payload, sizeof(fn));
    return fn;
  }

  inline void Value::set_prim(Prim fn) {
    release();
    uint64_t payload = 0;
    std::memcpy(&payload, &fn, sizeof(fn));
    bits = kTagPrim | (payload & kPayloadMask);
  }

  inline CFunc Value::get_cfunc() const {
    CFunc fn;
    uint64_t payload = bits & kPayloadMask;
    std::memcpy(&fn, &payload, sizeof(fn));
    return fn;
  }

  inline void Value::set_cfunc(CFunc fn) {
    release();
    uint64_t payload = 0;
    std::memcpy(&payload, &fn, sizeof(fn));
    bits = kTagCFunc | (payload & kPayloadMask);
  }

  inline void Value::retain() {
    Type t = get_type();
    if (!is_refcounted(t)) return;
    retain_payload(t, payload_ptr());
  }

  inline void Value::release() {
    Type t = get_type();
    if (!is_refcounted(t)) return;
    release_payload(t, payload_ptr());
    bits = kTagNil;
  }

  inline auto Value::is_refcounted(Type t) -> bool {
    // Use a constexpr lookup table indexed by the Type enum value for
    // faster, branch-free checks and better inlining opportunities.
    static constexpr bool kIsRefcounted[] = {
      /*TNIL*/ false,
      /*TPAIR*/ true,
      /*TNUMBER*/ false,
      /*TSTRING*/ true,
      /*TSYMBOL*/ true,
      /*TFUNC*/ true,
      /*TMACRO*/ true,
      /*TPRIM*/ false,
      /*TCFUNC*/ false,
      /*TENV*/ true
    };
    size_t idx = static_cast<size_t>(t);
    return idx < (sizeof(kIsRefcounted)/sizeof(kIsRefcounted[0])) ? kIsRefcounted[idx] : false;
  }

  inline void Value::retain_payload(Type t, void* p) {
    if (!p) return;
    auto *rc = static_cast<RcBase*>(p);
    rc->inc_ref();
  }

  class PairData : public RcBase { public: Value car; Value cdr; };

  // Function and macro runtime representations used by the evaluator.
  //
  // FuncData fields:
  // - params, body: AST nodes pointing to parameter list and function body
  // - closure_env: captured lexical environment
  // - call_count: a simple call counter used to decide when to JIT compile
  // - num_call_count: counter for pure numeric calls
  // - compiled_code: a void* that holds the machine-code pointer returned by
  //                  the JITCompiler after successful compilation (nullptr if not compiled)
  class FuncData : public RcBase { public: Value params; Value body; Env *closure_env = nullptr; size_t call_count = 0; size_t num_call_count = 0; void* compiled_code = nullptr; bool jit_failed = false; };

  // MacroData: macros are expanded by the interpreter at compile-time (no JIT)
  class MacroData : public RcBase { public: Value params; Value body; Env *closure_env = nullptr; };

} // namespace vdlisp

#endif // VDLISP__NANBOX_HPP