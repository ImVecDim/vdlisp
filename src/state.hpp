#ifndef VDLISP__STATE_HPP
#define VDLISP__STATE_HPP

// ============================================================================
// 内部头文件：SlabPool、ListBuilder、State::PoolData 等实现细节。
// vdlisp.hpp 已包含 State 的公共接口定义。
// ============================================================================

#include "../include/vdlisp.hpp"
#include <memory>
#include <vector>

namespace vdlisp {

// Slab 分配器：泛型模板版，支持 PairData / StringData / FuncData / MacroData。
// 顺序分配的对象落在一块连续内存中，遍历时利用缓存行预取。
// 目标类型的 operator delete 已被改写为空，slab 块在 shutdown 时统一回收。
template <typename T>
class SlabPool {
    static constexpr size_t kBlockSize = 16384;
    struct Block {
        alignas(alignof(T)) char data[kBlockSize];
    };
    std::vector<std::unique_ptr<Block>> blocks_;
    size_t used_ = kBlockSize;
  public:
    template <typename... Args>
    auto alloc(Args&&... args) -> T * {
        if (blocks_.empty() || used_ + sizeof(T) > kBlockSize) {
            blocks_.push_back(std::make_unique<Block>());
            used_ = 0;
        }
        if constexpr (alignof(T) > 1) {
            if (used_ > 0)
                used_ = (used_ + alignof(T) - 1) & ~(alignof(T) - 1);
        }
        auto *p = reinterpret_cast<T *>(blocks_.back()->data + used_);
        used_ += sizeof(T);
        return new (p) T(std::forward<Args>(args)...);
    }
    void purge() { blocks_.clear(); blocks_.shrink_to_fit(); used_ = kBlockSize; }
};

// State 内部实现（PIMPL）：包含所有 SlabPool 成员。
struct State::PoolData {
    SlabPool<PairData> pair_pool;
    SlabPool<StringData> string_pool;
    SlabPool<FuncData> func_pool;
    SlabPool<MacroData> macro_pool;
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

// 求值上下文 RAII 守卫：保存/恢复 State::current_expr
struct ExprGuard {
    State &S;
    Value prev;
    bool &ok;
    ExprGuard(State &S, const Value &expr, bool &ok) noexcept
        : S(S), prev(std::exchange(S.current_expr, expr)), ok(ok) {}
    ~ExprGuard() { if (ok) S.current_expr = std::move(prev); }
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
