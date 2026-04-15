#ifndef VDLISP__REQUIRE_HPP
#define VDLISP__REQUIRE_HPP

#include "vdlisp.hpp"

namespace vdlisp {

// 注册模块加载内建 `require`。
auto register_require(State &S) -> void;

} // namespace vdlisp

#endif // VDLISP__REQUIRE_HPP
