#ifndef VDLISP__COUNTED_HPP
#define VDLISP__COUNTED_HPP

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

namespace vdlisp
{

// A tiny reference-counted handle intended to replace std::shared_ptr in this project.
//
// Semantics (as requested):
// - counted<T> NEVER deletes / destroys the controlled object.
// - On last-release it only frees the reference counter.
//
// Layout goal:
// - The handle stores the controlled value T (in this project typically a raw pointer
//   like Value* / Env*) plus one extra machine word (size_t).
// - No deleter or extra control-block pointer stored in the handle.
//
// NOTE:
// To preserve shared ownership semantics across copies while keeping the handle size
// to sizeof(T) + sizeof(size_t), the reference counter itself is heap-allocated and
// the handle stores the counter *address* inside the size_t field.

template <class T>
class counted
{
public:
  using value_type = T;

  constexpr counted() noexcept : value_{}, ref_storage_{0} {}
  constexpr counted(std::nullptr_t) noexcept : value_{}, ref_storage_{0} {}

  explicit counted(T v) : value_{v}, ref_storage_{0}
  {
    if (value_) {
      ref_storage_ = reinterpret_cast<std::size_t>(new std::size_t(1));
    }
  }

  counted(const counted &other) noexcept : value_{other.value_}, ref_storage_{other.ref_storage_}
  {
    inc_ref();
  }

  counted(counted &&other) noexcept : value_{other.value_}, ref_storage_{other.ref_storage_}
  {
    other.value_ = T{};
    other.ref_storage_ = 0;
  }

  auto operator=(const counted &other) noexcept -> counted &
  {
    if (this == &other) return *this;
    dec_ref();
    value_ = other.value_;
    ref_storage_ = other.ref_storage_;
    inc_ref();
    return *this;
  }

  auto operator=(counted &&other) noexcept -> counted &
  {
    if (this == &other) return *this;
    dec_ref();
    value_ = other.value_;
    ref_storage_ = other.ref_storage_;
    other.value_ = T{};
    other.ref_storage_ = 0;
    return *this;
  }

  auto operator=(std::nullptr_t) noexcept -> counted &
  {
    reset();
    return *this;
  }

  ~counted() { dec_ref(); }

  void reset() noexcept
  {
    dec_ref();
    value_ = T{};
    ref_storage_ = 0;
  }

  [[nodiscard]] auto get() const noexcept -> T { return value_; }

  [[nodiscard]] auto use_count() const noexcept -> std::size_t
  {
    auto *p = ref_ptr();
    return p ? *p : 0;
  }

  [[nodiscard]] explicit operator bool() const noexcept { return value_ != T{}; }

  // pointer-like ops (only enabled when T is a pointer type)
  template <class U = T, std::enable_if_t<std::is_pointer<U>::value, int> = 0>
  [[nodiscard]] auto operator->() const noexcept -> U { return value_; }

  template <class U = T, std::enable_if_t<std::is_pointer<U>::value, int> = 0>
  [[nodiscard]] auto operator*() const noexcept -> std::add_lvalue_reference_t<std::remove_pointer_t<U>>
  {
    return *value_;
  }

  [[nodiscard]] auto operator==(std::nullptr_t) const noexcept -> bool { return value_ == T{}; }
  [[nodiscard]] auto operator!=(std::nullptr_t) const noexcept -> bool { return value_ != T{}; }

  friend auto operator==(const counted &a, const counted &b) noexcept -> bool { return a.value_ == b.value_; }
  friend auto operator!=(const counted &a, const counted &b) noexcept -> bool { return a.value_ != b.value_; }

private:
  [[nodiscard]] auto ref_ptr() const noexcept -> std::size_t*
  {
    return ref_storage_ ? reinterpret_cast<std::size_t*>(ref_storage_) : nullptr;
  }

  void inc_ref() noexcept
  {
    auto *p = ref_ptr();
    if (p) { ++(*p); }
  }

  void dec_ref() noexcept
  {
    auto *p = ref_ptr();
    if (!p) return;
    if (--(*p) == 0) {
      delete p;
    }
  }

  T value_;
  std::size_t ref_storage_;
};

static_assert(sizeof(counted<void*>) == sizeof(void*) + sizeof(std::size_t), "counted<T> size must be sizeof(T)+8 bytes (on 64-bit)");

} // namespace vdlisp

#endif // VDLISP__COUNTED_HPP
