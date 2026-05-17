#pragma once
#ifndef VDLISP__VDLISP_HPP
#define VDLISP__VDLISP_HPP

// ============================================================================
// vdlisp 公共 API 头文件
// 供外部使用者 #include，暴露解释器的全部对外接口。
// ============================================================================

#include <array>
#include <bit>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <boost/container/small_vector.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <boost/unordered/unordered_flat_map.hpp>


// ---- DLL 导出/导入宏 ----
// 构建动态库时导出符号；静态链接时宏为空（MinGW 无需 dllimport 即可正确解析）。

  #ifdef _WIN32
    #define VDLISP_API __declspec(dllexport)
  #else
    #define VDLISP_API __attribute__((visibility("default")))
  #endif


namespace vdlisp {

// ---- 前向声明 ----

class Value;
class PairData;
class StringData;
class FuncData;
class MacroData;
class State;
class Env;

// ---- 类型定义 ----

using Prim = Value (*)(State &, const Value &, Env *);
using CFunc = Value (*)(State &, const Value &);

enum Type : uint8_t {
    TNIL, TPAIR, TNUMBER, TSTRING, TSYMBOL,
    TFUNC, TMACRO, TPRIM, TCFUNC
};

// ---- 位操作辅助 ----

namespace detail {
inline constexpr auto double_to_bits(double value) noexcept -> uint64_t { return std::bit_cast<uint64_t>(value); }
inline constexpr auto bits_to_double(uint64_t bits) noexcept -> double { return std::bit_cast<double>(bits); }
} // namespace detail

// ---- 源码位置（原 State::SourceLoc） ----

struct SourceLoc {
    std::string file;
    size_t line = 0;
    size_t col = 0;
    std::string label;
};

// ---- 引用计数基础 ----

struct RcBase {
    size_t refs_{1};
    RcBase() noexcept = default;
    ~RcBase() noexcept = default;
    RcBase(const RcBase &) = delete;
    RcBase &operator=(const RcBase &) = delete;

    [[gnu::always_inline]] inline void inc_ref() noexcept { ++refs_; }
    [[gnu::always_inline]] inline size_t dec_ref() noexcept { return --refs_; }
};

// ---- StringData ----

class StringData : public RcBase {
  public:
    StringData() = default;
    explicit StringData(std::string_view s) : value(s) {}
    std::string value;
    static void operator delete(void *p) noexcept {}  // slab 分配
};

// ---- intrusive_ptr ADL 钩子前向声明 ----
// Env 仅需不完整类型即可声明；必须在 Env 类体之前，
// 否则 intrusive_ptr<Env> 成员实例化时两阶段查找失败。
inline void intrusive_ptr_add_ref(Env *e) noexcept;
inline void intrusive_ptr_release(Env *e) noexcept;
inline void retain_env(Env *e) noexcept;
inline void release_env(Env *e) noexcept;

// ---- Env（作用域环境） ----

class VDLISP_API Env : public RcBase {
  public:
    boost::unordered_flat_map<std::string, Value> map;
    boost::intrusive_ptr<Env> parent;
};

// ---- Env 引用计数 / intrusive_ptr 定义 ----

[[gnu::always_inline]] inline void retain_env(Env *e) noexcept {
    if (e) e->inc_ref();
}
[[gnu::always_inline]] inline void release_env(Env *e) noexcept {
    if (e && e->dec_ref() == 0) delete e;
}
[[gnu::always_inline]] inline void intrusive_ptr_add_ref(Env *e) noexcept { e->inc_ref(); }
[[gnu::always_inline]] inline void intrusive_ptr_release(Env *e) noexcept { if (e->dec_ref() == 0) delete e; }

struct EnvGuard {
    explicit EnvGuard(Env *e = nullptr) noexcept : e_(e) {}
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

// ============================================================================
// Value：NaN-boxing 值表示
// ============================================================================

class VDLISP_API Value {
  public:
    static constexpr uint64_t kNaNMask = 0x7FF0000000000000ULL;
    static constexpr uint64_t kTagMask = kNaNMask | 0x000F000000000000ULL;
    static constexpr uint64_t kPayloadMask = 0x0000FFFFFFFFFFFFULL;

    static constexpr uint64_t kTagNil    = kNaNMask | 0x0000000000000000ULL;
    static constexpr uint64_t kTagPair   = kNaNMask | 0x0001000000000000ULL;
    static constexpr uint64_t kTagString = kNaNMask | 0x0002000000000000ULL;
    static constexpr uint64_t kTagSymbol = kNaNMask | 0x0003000000000000ULL;
    static constexpr uint64_t kTagFunc   = kNaNMask | 0x0004000000000000ULL;
    static constexpr uint64_t kTagMacro  = kNaNMask | 0x0005000000000000ULL;
    static constexpr uint64_t kTagPrim   = kNaNMask | 0x0006000000000000ULL;
    static constexpr uint64_t kTagCFunc  = kNaNMask | 0x0007000000000000ULL;

    Value() : bits(kTagNil) {}
    explicit Value(Type t);
    Value(std::nullptr_t) : bits(kTagNil) {}
    Value(const Value &other);
    Value(Value &&other) noexcept;
    ~Value();

    auto operator=(const Value &other) noexcept -> Value &;
    auto operator=(Value &&other) noexcept -> Value &;
    auto operator=(std::nullptr_t) noexcept -> Value &;

    // ---- Getters ----

    [[nodiscard]] inline auto get_type() const noexcept -> Type {
        if ((bits & kNaNMask) != kNaNMask) [[unlikely]]
            return TNUMBER;
        static constexpr std::array kTagMap = {
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

    [[nodiscard]] auto type_name() const noexcept -> std::string_view;
    auto to_repr(State &S, std::string &out) const -> void;

    // ---- Setters ----

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

    template <uint64_t Tag, typename DataT>
    inline auto get_payload_raw() const noexcept -> DataT *;

    template <uint64_t Tag, typename DataT>
    inline void set_payload_raw(DataT *ptr) noexcept;

    template <uint64_t Tag, typename Fn>
    inline auto get_fn_raw() const noexcept -> Fn;

    template <uint64_t Tag, typename Fn>
    inline void set_fn_raw(Fn fn) noexcept;

    uint64_t bits;
};

// ---- Value 内联实现 ----

inline auto Value::get_number() const noexcept -> double {
    return std::bit_cast<double>(bits);
}

inline auto Value::set_number(double value) noexcept -> void {
    release();
    bits = std::bit_cast<uint64_t>(value);
    if ((bits & kNaNMask) == kNaNMask)
        bits = 0;
}

template <uint64_t Tag, typename DataT>
[[gnu::always_inline]] inline auto Value::get_payload_raw() const noexcept -> DataT * {
    return reinterpret_cast<DataT *>(bits & kPayloadMask);
}

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
    uint64_t payload = bits & kPayloadMask;
    return std::bit_cast<Fn>(payload);
}

template <uint64_t Tag, typename Fn>
inline auto Value::set_fn_raw(Fn fn) noexcept -> void {
    release();
    uint64_t payload = std::bit_cast<uint64_t>(fn);
    bits = Tag | (payload & kPayloadMask);
}

[[gnu::always_inline]] inline auto Value::get_pair() const noexcept -> PairData * {
    return get_payload_raw<kTagPair, PairData>();
}
inline auto Value::set_pair(PairData *ptr) noexcept -> void { set_payload_raw<kTagPair, PairData>(ptr); }

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

inline auto Value::get_func() const noexcept -> FuncData * { return get_payload_raw<kTagFunc, FuncData>(); }
inline auto Value::set_func(FuncData *ptr) noexcept -> void { set_payload_raw<kTagFunc, FuncData>(ptr); }
inline auto Value::get_macro() const noexcept -> MacroData * { return get_payload_raw<kTagMacro, MacroData>(); }
inline auto Value::set_macro(MacroData *ptr) noexcept -> void { set_payload_raw<kTagMacro, MacroData>(ptr); }
inline auto Value::get_prim() const noexcept -> Prim { return get_fn_raw<kTagPrim, Prim>(); }
inline auto Value::set_prim(Prim fn) noexcept -> void { set_fn_raw<kTagPrim, Prim>(fn); }
inline auto Value::get_cfunc() const noexcept -> CFunc { return get_fn_raw<kTagCFunc, CFunc>(); }
inline auto Value::set_cfunc(CFunc fn) noexcept -> void { set_fn_raw<kTagCFunc, CFunc>(fn); }

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
    return static_cast<size_t>(t) < std::size(kIsRefcounted) && kIsRefcounted[static_cast<size_t>(t)];
}

[[gnu::always_inline]] inline void Value::retain_payload(Type t, void *p) noexcept {
    if (p) static_cast<RcBase *>(p)->inc_ref();
}

// ---- PairData ----
// operator delete 为空：PairData 由 SlabPool 管理生命周期，
// 引用计数归零时仅调用析构函数释放子对象，内存由 pool 统一回收。

class PairData : public RcBase {
  public:
    Value car;
    Value cdr;
    static void operator delete(void *p) noexcept {}
};

// ---- FuncData ----
// 析构函数负责释放闭包环境引用；operator delete 为空（slab 分配）。

class FuncData : public RcBase {
  public:
    Value params;
    Value body;
    Env *closure_env = nullptr;
    ~FuncData() {
        if (closure_env) { release_env(closure_env); closure_env = nullptr; }
    }
    static void operator delete(void *p) noexcept {}
};

// ---- MacroData ----
// 同理，环境释放移入析构函数。

class MacroData : public RcBase {
  public:
    Value params;
    Value body;
    Env *closure_env = nullptr;
    ~MacroData() {
        if (closure_env) { release_env(closure_env); closure_env = nullptr; }
    }
    static void operator delete(void *p) noexcept {}
};

// ============================================================================
// 异常类型：LispError
// ============================================================================

struct VDLISP_API LispError : public std::runtime_error {
    SourceLoc loc;
    using Chain = boost::container::small_vector<SourceLoc, 4>;
    Chain call_chain;
    bool has_loc = false;

    explicit LispError(const std::string &msg)
        : std::runtime_error(msg), has_loc(false) {}
    LispError(SourceLoc loc, const std::string &msg)
        : std::runtime_error(msg), loc(std::move(loc)), has_loc(true) {}
    LispError(SourceLoc loc, const std::string &msg, Chain chain)
        : std::runtime_error(msg), loc(std::move(loc)), call_chain(std::move(chain)), has_loc(true) {}
};

// ============================================================================
// 辅助函数（内联，全部在公共 API 中暴露）
// ============================================================================

// 打印带源码定位的错误信息
VDLISP_API auto print_error_with_loc(const State &S, const SourceLoc &loc, const std::string &msg) -> void;

// 结构相等比较
[[nodiscard]] auto value_equal(const Value &a, const Value &b) -> bool;

// 数值参数检查
[[nodiscard]] [[gnu::always_inline]] inline auto require_number(const Value &v, const char *who) -> double {
    if (!v || v.get_type() != TNUMBER) [[unlikely]]
        throw LispError(std::string(who) + ": expected number, got " + std::string(v.type_name()));
    return v.get_number();
}

// ---- pair 访问 ----

template <auto Member>
[[nodiscard]] [[gnu::always_inline]] inline const Value& pair_access(const Value &p) noexcept {
    if (!p || p.get_type() != TPAIR) {
        static const Value kNil;
        return kNil;
    }
    return (p.get_pair()->*Member);
}
[[nodiscard]] [[gnu::always_inline]] inline const Value& pair_car(const Value &p) noexcept { return pair_access<&PairData::car>(p); }
[[nodiscard]] [[gnu::always_inline]] inline const Value& pair_cdr(const Value &p) noexcept { return pair_access<&PairData::cdr>(p); }

[[nodiscard]] [[gnu::always_inline]] inline auto is_pair(const Value &p) noexcept -> bool {
    return p && p.get_type() == TPAIR;
}
[[nodiscard]] [[gnu::always_inline]] inline auto is_symbol(const Value &p, const std::string &name) -> bool {
    return p && p.get_type() == TSYMBOL && *p.get_symbol() == name;
}

template <auto Member>
[[gnu::always_inline]] inline void pair_set(const Value &p, const Value &v) noexcept {
    if (!p || p.get_type() != TPAIR) return;
    p.get_pair()->*Member = v;
}
[[gnu::always_inline]] inline void pair_set_car(const Value &p, const Value &v) noexcept { pair_set<&PairData::car>(p, v); }
[[gnu::always_inline]] inline void pair_set_cdr(const Value &p, const Value &v) noexcept { pair_set<&PairData::cdr>(p, v); }

// 遍历 Lisp 列表
inline void foreach_lisp(const Value &list, auto&& F) {
    const Value *cur = &list;
    while (*cur && cur->get_type() == TPAIR) {
        PairData *pd = cur->get_pair();
        F(pd->car);
        cur = &pd->cdr;
    }
}

// 参数校验辅助
[[nodiscard]] [[gnu::always_inline]] inline auto require_unary_args(const Value &args, const char *name) -> Value {
    if (!args || pair_cdr(args))
        throw LispError(std::string(name) + " requires exactly one argument");
    return pair_car(args);
}

[[nodiscard]] [[gnu::always_inline]] inline auto require_binary_args(const Value &args, const char *name) -> std::pair<Value, Value> {
    if (!args || !pair_cdr(args) || pair_cdr(pair_cdr(args)))
        throw LispError(std::string(name) + " requires exactly two arguments");
    return {pair_car(args), pair_car(pair_cdr(args))};
}

[[nodiscard]] [[gnu::always_inline]] inline auto require_pair_arg(const Value &args, const char *name) -> Value {
    Value v = require_unary_args(args, name);
    if (v && v.get_type() != TPAIR)
        throw LispError(std::string(name) + " expects a pair");
    return v;
}

// 断开闭包对环境的引用
VDLISP_API void clear_closure_env(Value &v) noexcept;

// ============================================================================
// Unified public API typedef/alias  (State forward-declared above)
// ============================================================================

// 便捷地把一组 Value 拼成 Lisp 列表（定义在 state.hpp 中）
template <typename... Vs>
[[nodiscard]] auto list_of(State &S, Vs&&... vs) -> Value;

} // namespace vdlisp

#endif // VDLISP__VDLISP_HPP
