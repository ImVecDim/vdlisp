#ifndef VDLISP__VDLISP__HPP
#define VDLISP__VDLISP__HPP

#include "nanbox.hpp"
#include <cstddef>
#include <initializer_list>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace vdlisp {

// 透明哈希/等值比较，支持 string_view 在 unordered_map<string, T> 中查找
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
// 块列表使用 std::vector<std::unique_ptr<Block>>，避免 vector 扩容时指针移动。
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
        // 对齐（新块 used_=0 已对齐，跳过）
        if (used_ > 0)
            used_ = (used_ + alignof(PairData) - 1) & ~(alignof(PairData) - 1);
        auto *p = reinterpret_cast<PairData *>(blocks_.back()->data + used_);
        used_ += sizeof(PairData);
        return new (p) PairData();
    }
    void purge() { blocks_.clear(); used_ = kBlockSize; }
};

class State {
  public:
    // 运行时的核心对象：持有全局环境、符号表、源码映射与模块缓存。
    Env *global = nullptr;
    std::unordered_map<std::string, Value, StringHash, StringEqual> symbol_intern;

    State();

    // 尽力释放运行期引用，主要用于正常退出与泄漏检查。
    auto shutdown_and_purge_pools() -> void;

    // 各类 Lisp 值的统一构造入口。
    [[nodiscard]] auto make_nil() noexcept -> Value;
    [[nodiscard]] auto make_number(double n) noexcept -> Value;
    [[nodiscard]] auto make_string(const std::string &s) -> Value;
    [[nodiscard]] auto make_symbol(std::string_view s) -> Value;
    [[nodiscard]] auto make_pair(Value car, Value cdr) -> Value;
    [[nodiscard]] auto make_cfunc(const CFunc &fn) noexcept -> Value;
    [[nodiscard]] auto make_function(Value params, Value body, Env *env) -> Value;
    [[nodiscard]] auto make_prim(const Prim &fn) noexcept -> Value;
    [[nodiscard]] auto make_macro(Value params, Value body, Env *env) -> Value;

    // Env 与 Value 的底层分配封装，对上层隐藏具体内存表示。
    [[nodiscard]] auto make_env(Env *parent = nullptr) -> Env *;

    // 便捷的字符串列表构造器，主要给 argv 等宿主输入使用。
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

    // 解析、求值与调用构成解释器主入口。
    [[nodiscard]] auto parse(const std::string &src, const std::string &name = "(string)") -> Value;
    [[nodiscard]] auto parse_all(const std::string &src, const std::string &name = "(string)") -> Value;
    [[nodiscard]] auto eval(const Value &expr, Env *env) -> Value;
    [[nodiscard]] auto call(const Value &fn, const Value &args, Env *env = nullptr) -> Value;
    [[nodiscard]] auto do_list(const Value &body, Env *env) -> Value;

    // 给 AST 节点绑定源码位置，便于报错与宏展开追踪。
    struct SourceLoc {
        std::string file;
        size_t line = 0;
        size_t col = 0;
        std::string label;
    };
    auto set_source_loc(const Value &v, std::string_view file, size_t line, size_t col) -> void;
    auto get_source_loc(const Value &v, SourceLoc &out) const -> bool;

    // 当前正在求值的表达式；异常时故意保留，供顶层错误报告读取。
    Value current_expr;
    // Value 身份到源码位置的映射。
    std::unordered_map<uint64_t, SourceLoc> src_map;
    // 宏展开或函数调用传播过来的调用链，帮助定位“错误从哪里展开而来”。
    std::unordered_map<uint64_t, std::vector<SourceLoc>> src_call_chain_map;

    // 已载入源码文本，用于报错时回显源码行。
    std::unordered_map<std::string, std::string, StringHash, StringEqual> sources;
    // `require` 模块缓存，键尽量使用规范化路径。
    std::unordered_map<std::string, Value> loaded_modules;
    // 返回指定源码行；若源码不存在则返回 false。
    [[nodiscard]] auto get_source_line(std::string_view file, size_t line, std::string &out) const -> bool;

  private:
    // 具体对象的堆分配细节都收敛在这里。
    [[nodiscard]] auto alloc_string(const std::string &s) -> StringData *;
    // Allocation helpers take rvalue references to avoid an extra move
    [[nodiscard]] auto alloc_pair(Value &&car, Value &&cdr) -> PairData *;
    [[nodiscard]] auto alloc_func(Value &&params, Value &&body, Env *env) -> FuncData *;
    [[nodiscard]] auto alloc_macro(Value &&params, Value &&body, Env *env) -> MacroData *;

    // Pair 缓存行感知分配器。
    PairPool pair_pool;

    // Env 的底层分配口，和上层 make_env 分离便于后续替换策略。
    [[nodiscard]] auto alloc_env() -> Env *;

  public:
    // 对外常用的运行时辅助函数。
    [[nodiscard]] auto to_string(const Value &v) -> std::string;
    auto register_builtin(const std::string &name, const CFunc &fn) -> void;
    auto register_prim(const std::string &name, const Prim &fn) -> void;
    [[nodiscard]] auto get_bound(const std::string &name, Env *env) -> Value;
    [[nodiscard]] auto lookup(const std::string &name, Env *env) -> Value *;
    auto bind_global(const std::string &name, Value v) -> void;
    [[nodiscard]] auto bind(const Value &sym, Value v, Env *env) -> Value;
    [[nodiscard]] auto set(const Value &sym, Value v, Env *env) -> Value;
};

// 执行 JIT 机器码时，桥接函数通过它回到当前解释器状态。
extern State *jit_active_state;

// 便捷地把一组 Value 拼成 Lisp 列表。
[[nodiscard]] auto list_of(State &S, std::initializer_list<Value> items) -> Value;

// 工具：消除各处"尾插构造链表"的重复模式
struct ListBuilder {
    Value head;
    Value *last = &head;
    void add(State &S, Value &&v) {
        *last = S.make_pair(std::move(v), Value());
        last = &(*last).get_pair()->cdr;
    }
    [[nodiscard]] Value done() && { return std::move(head); }
};

} // namespace vdlisp

#endif // VDLISP__VDLISP__HPP
