#include "nanbox.hpp"
#include "jit/jit.hpp"
#include <unordered_map>
#include <vector>
#include <iostream>
#include <sstream>

using namespace vdlisp;

// JIT compiler instance is provided by `global_jit` declared in the JIT header.
// The concrete `JITCompiler global_jit` definition lives in `src/jit/jit.cpp`.

// -------------------- Value implementation --------------------

Value::Value(Type t)
{
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

Value::Value(const Value &other) : bits(other.bits)
{
  retain();
}

Value::Value(Value &&other) noexcept : bits(other.bits)
{
  other.bits = kTagNil;
}

Value::~Value() {
  release();
}

auto Value::operator=(const Value &other) -> Value&
{
  if (this == &other) return *this;
  release();
  bits = other.bits;
  retain();
  return *this;
}

auto Value::operator=(Value &&other) noexcept -> Value&
{
  if (this == &other) return *this;
  release();
  bits = other.bits;
  other.bits = kTagNil;
  return *this;
}

auto Value::operator=(std::nullptr_t) -> Value&
{
  release();
  bits = kTagNil;
  return *this;
}

auto Value::get_number() const -> double
{
  // Reinterpret bits as double
  double result;
  static_assert(sizeof(double) == sizeof(bits), "Double must be 64-bit");
  std::memcpy(&result, &bits, sizeof(result));
  return result;
}

void Value::set_number(double value)
{
  release();
  // Store the double bit pattern directly
  std::memcpy(&bits, &value, sizeof(bits));
  // Ensure it doesn't accidentally match our NaN tagging scheme
  if ((bits & kNaNMask) == kNaNMask) {
    // If it's a NaN that conflicts with our tagging, replace with 0.0
    bits = 0;
  }
}

auto Value::get_pair() const -> PairData*
{
  return reinterpret_cast<PairData*>(bits & kPayloadMask);
}

void Value::set_pair(PairData* ptr)
{
  release();
  bits = kTagPair | (reinterpret_cast<uint64_t>(ptr) & kPayloadMask);
}

auto Value::get_string() const -> std::string*
{
  auto *sd = reinterpret_cast<StringData*>(bits & kPayloadMask);
  return sd ? &sd->value : nullptr;
}

void Value::set_string(StringData* ptr)
{
  release();
  bits = kTagString | (reinterpret_cast<uint64_t>(ptr) & kPayloadMask);
}

auto Value::get_symbol() const -> std::string*
{
  auto *sd = reinterpret_cast<StringData*>(bits & kPayloadMask);
  return sd ? &sd->value : nullptr;
}

void Value::set_symbol(StringData* ptr)
{
  release();
  bits = kTagSymbol | (reinterpret_cast<uint64_t>(ptr) & kPayloadMask);
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
auto Value::get_func() const -> FuncData* {
    auto* func = reinterpret_cast<FuncData*>(bits & kPayloadMask);
    return func;
}

// Macro handling is executed by the interpreter at expansion time. Macros are
// tracked with a call counter (for diagnostics) but are not JIT compiled.
auto Value::get_macro() const -> MacroData* {
    return reinterpret_cast<MacroData*>(bits & kPayloadMask);
}

void Value::set_func(FuncData* ptr) {
  release();
    bits = kTagFunc | (reinterpret_cast<uint64_t>(ptr) & kPayloadMask);
}

void Value::set_macro(MacroData* ptr) {
  release();
    bits = kTagMacro | (reinterpret_cast<uint64_t>(ptr) & kPayloadMask);
}

Prim Value::get_prim() const
{
  Prim fn;
  uint64_t payload = bits & kPayloadMask;
  std::memcpy(&fn, &payload, sizeof(fn));
  return fn;
}

void Value::set_prim(Prim fn)
{
  release();
  uint64_t payload = 0;
  std::memcpy(&payload, &fn, sizeof(fn));
  bits = kTagPrim | (payload & kPayloadMask);
}

CFunc Value::get_cfunc() const
{
  CFunc fn;
  uint64_t payload = bits & kPayloadMask;
  std::memcpy(&fn, &payload, sizeof(fn));
  return fn;
}

void Value::set_cfunc(CFunc fn)
{
  release();
  uint64_t payload = 0;
  std::memcpy(&payload, &fn, sizeof(fn));
  bits = kTagCFunc | (payload & kPayloadMask);
}

void Value::retain()
{
  Type t = get_type();
  if (!is_refcounted(t)) return;
  retain_payload(t, payload_ptr());
}

void Value::release()
{
  Type t = get_type();
  if (!is_refcounted(t)) return;
  release_payload(t, payload_ptr());
  bits = kTagNil;
}

auto Value::is_refcounted(Type t) -> bool
{
  switch (t) {
    case TPAIR:
    case TSTRING:
    case TSYMBOL:
    case TFUNC:
    case TMACRO:
      return true;
    default:
      return false;
  }
}

void Value::retain_payload(Type t, void* p)
{
  if (!p) return;
  auto *rc = static_cast<RcBase*>(p);
  ++rc->refs;
}

void Value::release_payload(Type t, void* p)
{
  if (!p) return;
  auto *rc = static_cast<RcBase*>(p);
  if (--rc->refs != 0) return;

  switch (t) {
    case TPAIR:
      delete static_cast<PairData*>(p);
      break;
    case TSTRING:
      delete static_cast<StringData*>(p);
      break;
    case TSYMBOL:
      delete static_cast<StringData*>(p);
      break;
    case TFUNC: {
      auto *fd = static_cast<FuncData*>(p);
      if (fd->compiled_code) {
        global_jit.releaseFunctionCode(fd->compiled_code);
        fd->compiled_code = nullptr;
      }
      delete fd;
      break;
    }
    case TMACRO:
      delete static_cast<MacroData*>(p);
      break;
    default:
      break;
  }
}

// High-level helpers centralized on Value
auto Value::type_name() const -> std::string
{
  switch (get_type()) {
    case TNIL: return "nil";
    case TPAIR: return "pair";
    case TNUMBER: return "number";
    case TSTRING: return "string";
    case TSYMBOL: return "symbol";
    case TFUNC: {
      // Inspect the stored FuncData pointer without triggering JIT or side-effects
      auto* fd = reinterpret_cast<FuncData*>(bits & kPayloadMask);
      return (fd && fd->compiled_code) ? "jit_func" : "function";
    }
    case TMACRO: return "macro";
    case TPRIM: return "prim";
    case TCFUNC: return "cfunction";
    case TENV: return "env";
    default: return "?";
  }
}

auto Value::to_repr(State &S) const -> std::string
{
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
        s += pd->car ? pd->car->to_repr(S) : std::string("nil");
        Value cur = pd->cdr;
        while (cur && cur->get_type() == TPAIR) {
          s += " ";
          PairData *cpd = cur->get_pair();
          s += cpd->car ? cpd->car->to_repr(S) : std::string("nil");
          cur = cpd->cdr;
        }
        if (cur) {
          s += " . ";
          s += cur->to_repr(S);
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
    case TENV:
      return "<env>";
    case TFUNC: {
      auto* fd = reinterpret_cast<FuncData*>(bits & kPayloadMask);
      return (fd && fd->compiled_code) ? "<jit_func>" : "<function>";
    }
    default:
      return "<?>";
  }
}