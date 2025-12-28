#ifndef VDLISP__SPTR_HPP
#define VDLISP__SPTR_HPP

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

namespace vdlisp
{

// A tiny reference-counted handle intended to replace std::shared_ptr in this project.
//
// Layout goal:
// - The handle stores the controlled value pointer plus one extra machine word (size_t).
// - The reference counter itself is heap-allocated and the handle stores the counter address.

template <class T>
class sptr
{
public:
  using value_type = T;

  static_assert(!std::is_pointer<T>::value, "sptr<T> expects a non-pointer T; use sptr<Value> instead of sptr<Value*>");

  constexpr sptr() noexcept : value_{}, ref_storage_{0} {}
  constexpr sptr(std::nullptr_t) noexcept : value_{}, ref_storage_{0} {}

  explicit sptr(T *v) : value_{v}, ref_storage_{0}
  {
    if (value_) {
      ref_storage_ = reinterpret_cast<std::size_t>(new std::size_t(1));
    }
  }

  sptr(const sptr &other) noexcept : value_{other.value_}, ref_storage_{other.ref_storage_}
  {
    inc_ref();
  }

  sptr(sptr &&other) noexcept : value_{other.value_}, ref_storage_{other.ref_storage_}
  {
    other.value_ = nullptr;
    other.ref_storage_ = 0;
  }

  auto operator=(const sptr &other) noexcept -> sptr &
  {
    if (this == &other) return *this;
    dec_ref();
    value_ = other.value_;
    ref_storage_ = other.ref_storage_;
    inc_ref();
    return *this;
  }

  auto operator=(sptr &&other) noexcept -> sptr &
  {
    if (this == &other) return *this;
    dec_ref();
    value_ = other.value_;
    ref_storage_ = other.ref_storage_;
    other.value_ = nullptr;
    other.ref_storage_ = 0;
    return *this;
  }

  auto operator=(std::nullptr_t) noexcept -> sptr &
  {
    reset();
    return *this;
  }

  ~sptr() { dec_ref(); }

  void reset() noexcept
  {
    dec_ref();
    value_ = nullptr;
    ref_storage_ = 0;
  }

  [[nodiscard]] auto get() const noexcept -> T* { return value_; }

  [[nodiscard]] auto use_count() const noexcept -> std::size_t
  {
    auto *p = ref_ptr();
    return p ? *p : 0;
  }

  [[nodiscard]] explicit operator bool() const noexcept { return value_ != nullptr; }

  [[nodiscard]] auto operator->() const noexcept -> T* { return value_; }
  [[nodiscard]] auto operator*() const noexcept -> std::add_lvalue_reference_t<T>
  {
    return *value_;
  }

  [[nodiscard]] auto operator==(std::nullptr_t) const noexcept -> bool { return value_ == nullptr; }
  [[nodiscard]] auto operator!=(std::nullptr_t) const noexcept -> bool { return value_ != nullptr; }

  friend auto operator==(const sptr &a, const sptr &b) noexcept -> bool { return a.value_ == b.value_; }
  friend auto operator!=(const sptr &a, const sptr &b) noexcept -> bool { return a.value_ != b.value_; }

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
      T *to_delete = value_;
      value_ = nullptr;
      ref_storage_ = 0;
      delete to_delete;
      delete p;
    }
  }

  T *value_;
  std::size_t ref_storage_;
};

static_assert(sizeof(sptr<void>) == sizeof(void*) + sizeof(std::size_t), "sptr<T> size must be sizeof(ptr)+sizeof(size_t)");

} // namespace vdlisp

#endif // VDLISP__SPTR_HPP
