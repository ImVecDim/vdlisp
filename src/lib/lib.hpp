#ifndef VDLISP__LIB_HPP
#define VDLISP__LIB_HPP

#include "core.hpp"
#include "require.hpp"

namespace vdlisp {
class State;
}

namespace vdlisp {
// 注册所有核心库函数和内建
inline auto register_lib(State &S) -> void {
    register_core(S);
    register_require(S);
}
}

#endif // VDLISP__LIB_HPP
