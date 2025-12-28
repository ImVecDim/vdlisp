#ifndef VDLISP__NANBOX_HPP
#define VDLISP__NANBOX_HPP

#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>

#include "sptr.hpp"


namespace vdlisp
{

  class Value;
  using Ptr = sptr<Value>;
  class State;
  class Env;

  // Use plain function pointer types for primitives and C functions to avoid
  // the overhead of std::function (allocations / type-erasure / indirect calls).
  using Prim = Ptr (*)(State &, Ptr, sptr<Env>);
  using CFunc = Ptr (*)(State &, Ptr);

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
      uint64_t bits;
      memcpy(&bits, &value, sizeof(double));
      return bits;
    }

    inline auto bits_to_double(uint64_t bits) -> double {
      double value;
      memcpy(&value, &bits, sizeof(double));
      return value;
    }
  } // namespace detail

  class PairData { public: Ptr car; Ptr cdr; };

  // Function and macro runtime representations used by the evaluator.
  //
  // FuncData fields:
  // - params, body: AST nodes pointing to parameter list and function body
  // - closure_env: captured lexical environment
  // - call_count: a simple call counter used to decide when to JIT compile
  // - num_call_count: counter for pure numeric calls
  // - compiled_code: a void* that holds the machine-code pointer returned by
  //                  the JITCompiler after successful compilation (nullptr if not compiled)
  class FuncData { public: Ptr params; Ptr body; sptr<Env> closure_env; size_t call_count = 0; size_t num_call_count = 0; void* compiled_code = nullptr; bool jit_failed = false; };

  // MacroData: macros are expanded by the interpreter at compile-time (no JIT)
  class MacroData { public: Ptr params; Ptr body; sptr<Env> closure_env; };

  class Env
  {
  public:
    std::unordered_map<std::string, Ptr> map;
    sptr<Env> parent;
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
    ~Value();

    // Getters

    // Hot function, inlined for performance
    [[nodiscard]] inline auto get_type() const -> Type {
      // Check if it's a canonical NaN (numbers)
      if ((bits & kNaNMask) != kNaNMask) {
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

    // High-level helpers
    [[nodiscard]] auto type_name() const -> std::string;
    auto to_repr(State &S) const -> std::string;

    // Setters
    void set_number(double value);
    void set_pair(PairData* ptr);
    void set_string(std::string* ptr);
    void set_symbol(std::string* ptr);
    void set_func(FuncData* ptr);
    void set_macro(MacroData* ptr);
    void set_prim(Prim fn);
    void set_cfunc(CFunc fn);

  private:
    // Use NaN-boxing to store all value types in a single 64-bit integer
    // Runtime assumptions are documented in the .cpp implementation.
    uint64_t bits;
  };

} // namespace vdlisp

#endif // VDLISP__NANBOX_HPP