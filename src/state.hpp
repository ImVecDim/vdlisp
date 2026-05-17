#ifndef VDLISP__STATE_HPP
#define VDLISP__STATE_HPP

// ============================================================================
// 内部头文件：完整的 State 类定义（与内部实现辅助类型）
// 包含了公共 API 头文件，并在其上叠加了私有成员、PairPool、StringHash 等。
// ============================================================================

#include "../include/vdlisp.hpp"
#include <memory>
#include <string>
#include <unordered_map>

namespace vdlisp {

// ---- 内部辅助类型 ----

// 透明哈希，支持 string_view 在 unordered_map<string, T> 中查找
struct StringHash {
    using is_transparent = void;
    auto operator()(std::string_view sv) const { return std::hash<std::string_view>{}(sv); }
    auto operator()(const std::string &s) const { return std::hash<std::string>{}(s); }
};
struct StringEqual {
    using is_transparent = void;
    auto operator()(std::string_view a, const std::string &b) const { return a == b; }
    auto operator()(const std::string &a, std::string_view b) const { return a == b; }
    auto operator()(const std::string &a, const std::string &b) const { return a == b; }
};

// Slab 分配器：PairData 的缓存行感知 allocator。
// 顺序分配的 pair 落在一块连续内存中，链表遍历时利用缓存行预取。
// PairData 的 operator delete 已被改写为空，slab 块在 shutdown 时统一回收。
class PairPool {
    static constexpr size_t kBlockSize = 16384;
    struct Block {
        alignas(alignof(PairData)) char data[kBlockSize];
    };
    std::vector<std::unique_ptr<Block>> blocks_;
    size_t used_ = kBlockSize;
  public:
    auto alloc() -> PairData * {
        if (blocks_.empty() || used_ + sizeof(PairData) > kBlockSize) {
            blocks_.push_back(std::make_unique<Block>());
            used_ = 0;
        }
        if (used_ > 0)
            used_ = (used_ + alignof(PairData) - 1) & ~(alignof(PairData) - 1);
        auto *p = reinterpret_cast<PairData *>(blocks_.back()->data + used_);
        used_ += sizeof(PairData);
        return new (p) PairData();
    }
    void purge() { blocks_.clear(); used_ = kBlockSize; }
};

// ---- State：完整的解释器全局状态 ----

class VDLISP_API State {
  public:
    Env *global = nullptr;
    std::unordered_map<std::string_view, Value> symbol_intern;

    State();
    ~State();

    auto shutdown_and_purge_pools() -> void;

    // ---------- Factory methods ----------
    [[nodiscard]] auto make_nil() noexcept -> Value;
    [[nodiscard]] auto make_number(double n) noexcept -> Value;
    [[nodiscard]] auto make_string(const std::string &s) -> Value;
    [[nodiscard]] auto make_symbol(std::string_view s) -> Value;
    [[nodiscard]] auto make_pair(Value car, Value cdr) -> Value;
    [[nodiscard]] auto make_cfunc(const CFunc &fn) noexcept -> Value;
    [[nodiscard]] auto make_function(Value params, Value body, Env *env) -> Value;
    [[nodiscard]] auto make_prim(const Prim &fn) noexcept -> Value;
    [[nodiscard]] auto make_macro(Value params, Value body, Env *env) -> Value;

    [[nodiscard]] auto make_env(Env *parent = nullptr) -> Env *;

    // 便捷的字符串列表构造器
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
    [[nodiscard]] auto make_string_list(int argc, char **argv, int start = 0) -> Value;

    // ---------- 解析、求值与调用 ----------
    [[nodiscard]] auto parse(const std::string &src, const std::string &name = "(string)") -> Value;
    [[nodiscard]] auto parse_all(const std::string &src, const std::string &name = "(string)") -> Value;
    [[nodiscard]] auto eval(const Value &expr, Env *env) -> Value;
    [[nodiscard]] auto call(const Value &fn, const Value &args, Env *env = nullptr) -> Value;
    [[nodiscard]] auto do_list(const Value &body, Env *env) -> Value;

    // ---------- 源码位置 ----------
    auto set_source_loc(const Value &v, std::string_view file, size_t line, size_t col) -> void;
    auto get_source_loc(const Value &v, SourceLoc &out) const -> bool;

    // ---------- 公开字段 ----------
    Value current_expr;
    std::unordered_map<uint64_t, SourceLoc> src_map;
    std::unordered_map<uint64_t, LispError::Chain> src_call_chain_map;
    std::unordered_map<std::string, std::string, StringHash, StringEqual> sources;
    std::unordered_map<std::string, Value> loaded_modules;

    [[nodiscard]] auto get_source_line(std::string_view file, size_t line, std::string &out) const -> bool;

    // ---------- 辅助 ----------
    [[nodiscard]] auto to_string(const Value &v) -> std::string;
    auto register_builtin(const std::string &name, const CFunc &fn) -> void;
    auto register_prim(const std::string &name, const Prim &fn) -> void;
    [[nodiscard]] auto get_bound(const std::string &name, Env *env) -> Value;
    [[nodiscard]] auto lookup(const std::string &name, Env *env) -> Value *;
    auto bind_global(const std::string &name, Value v) -> void;
    [[nodiscard]] auto bind(const Value &sym, Value v, Env *env) -> Value;
    [[nodiscard]] auto set(const Value &sym, Value v, Env *env) -> Value;

  private:
    // 底层分配器
    [[nodiscard]] auto alloc_string(const std::string &s) -> StringData *;
    [[nodiscard]] auto alloc_pair(Value &&car, Value &&cdr) -> PairData *;
    [[nodiscard]] auto alloc_func(Value &&params, Value &&body, Env *env) -> FuncData *;
    [[nodiscard]] auto alloc_macro(Value &&params, Value &&body, Env *env) -> MacroData *;
    [[nodiscard]] auto alloc_env() -> Env *;

    PairPool pair_pool;
};

// 尾插构造链表的工具（需放在 State 定义之后，因为调用了 State::make_pair）
struct VDLISP_API ListBuilder {
    Value head;
    Value *last = &head;
    void add(State &S, Value &&v) {
        *last = S.make_pair(std::move(v), Value());
        last = &(*last).get_pair()->cdr;
    }
    [[nodiscard]] Value done() && { return std::move(head); }
};

// 可变参数模板 list_of（完美转发，消除 initializer_list 导致的拷贝）
template <typename... Vs>
[[nodiscard]] inline auto list_of(State &S, Vs&&... vs) -> Value {
    if constexpr (sizeof...(vs) == 0) return {};
    Value head;
    Value *tail = &head;
    ((*tail = S.make_pair(Value(std::forward<Vs>(vs)), Value()),
      tail = &(*tail).get_pair()->cdr), ...);
    return head;
}

} // namespace vdlisp

#endif // VDLISP__STATE_HPP
