#ifndef JIT_JIT_IR_BUILDER_HPP
#define JIT_JIT_IR_BUILDER_HPP

#include <string>

namespace llvm {
class Module;
class LLVMContext;
class Function;
} // namespace llvm
namespace vdlisp {
class FuncData;
class Value;
}

// 粗粒度判定某个函数体是否值得尝试走 JIT。
auto can_attempt_jit_compile(vdlisp::FuncData *func) -> bool;
// 把 Lisp 函数体翻译成 LLVM IR；失败时可退回解释器桩函数。
auto build_func_ir(vdlisp::FuncData *func, llvm::Module &M, llvm::LLVMContext &context, const std::string &name) -> llvm::Function *;

#endif // JIT_JIT_IR_BUILDER_HPP
