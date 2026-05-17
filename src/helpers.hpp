#ifndef VDLISP__HELPERS_HPP
#define VDLISP__HELPERS_HPP

// 内部辅助头文件：仅保留不在公共 API 中的函数声明。
// 公共 API 部分的辅助函数（LispError、pair_car、foreach_lisp 等）
// 已移至 include/vdlisp.hpp。

#include "state.hpp"

namespace vdlisp {

// 打印带源码定位的错误信息，供 REPL 和批处理入口复用。
auto print_error_with_loc(const State &S, const SourceLoc &loc, const std::string &msg) -> void;

} // namespace vdlisp

#endif // VDLISP__HELPERS_HPP
