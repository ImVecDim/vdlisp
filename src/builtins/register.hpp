#ifndef VDLISP__BUILTINS_REGISTER_HPP
#define VDLISP__BUILTINS_REGISTER_HPP

#include "core.hpp"
#include "math.hpp"
#include "require.hpp"

namespace vdlisp {
// 注册所有核心库函数和内建
inline auto register_lib(State &S) -> void {
    register_core(S);
    register_math(S);
    register_require(S);
}
}
#endif // VDLISP__BUILTINS_REGISTER_HPP
