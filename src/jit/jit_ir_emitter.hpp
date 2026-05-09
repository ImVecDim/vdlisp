#ifndef JIT_JIT_IR_EMITTER_HPP
#define JIT_JIT_IR_EMITTER_HPP

#include <llvm/IR/IRBuilder.h>
#include <string>
#include <unordered_map>

namespace llvm {
class AllocaInst;
class Function;
class LLVMContext;
class Value;
} // namespace llvm

namespace vdlisp {
class FuncData;
class Value;
} // namespace vdlisp

class JITIREmitter {
  public:
    // 负责把受支持的 Lisp 子集直接翻译成 double 型 LLVM IR。
    JITIREmitter(vdlisp::FuncData *func, llvm::Function *F, llvm::LLVMContext &context);
    auto emitExpr(const vdlisp::Value &expr) -> llvm::Value *;
    auto finalize() -> llvm::Function *;

  private:
    vdlisp::FuncData *func;
    llvm::Function *F;
    llvm::LLVMContext &context;
    llvm::IRBuilder<> ir;
    std::unordered_map<std::string, llvm::AllocaInst *> locals;
    std::unordered_map<std::string, int> param_index;

    // 局部变量第一次写入时在入口块里补 alloca，保证 SSA 之外仍可进行简单赋值。
    auto ensure_local(const std::string &name) -> llvm::AllocaInst *;
};

#endif // JIT_JIT_IR_EMITTER_HPP
