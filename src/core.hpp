#ifndef VDLISP__CORE_HPP
#define VDLISP__CORE_HPP

namespace vdlisp {
class State;
}

namespace vdlisp {
// 注册解释器启动后默认可用的内建函数与特殊形式。
auto register_core(State &S) -> void;
}

#endif // VDLISP__CORE_HPP