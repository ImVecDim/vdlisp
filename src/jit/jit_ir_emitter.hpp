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
    JITIREmitter(vdlisp::FuncData *func, llvm::Function *F, llvm::LLVMContext &context);
    auto emitExpr(const vdlisp::Value &expr) -> llvm::Value *;
    auto finalize() -> llvm::Function *;

  private:
    vdlisp::FuncData *func;
    llvm::Function *F;
    llvm::LLVMContext &context;
    llvm::IRBuilder<> ir;

    llvm::Type *dblTy;
    llvm::Constant *dblZero;
    llvm::Constant *dblOne;

    std::unordered_map<std::string, llvm::AllocaInst *> locals;
    std::unordered_map<std::string, int> param_index;

    auto ensure_local(const std::string &name) -> llvm::AllocaInst *;

    // 按语法形式拆分的子发射器，每个只负责一种 Lisp 子语言构造。
    auto emitSymbol(const vdlisp::Value &expr) -> llvm::Value *;
    auto emitLet(const vdlisp::Value &rest) -> llvm::Value *;
    auto emitCond(const vdlisp::Value &rest) -> llvm::Value *;
    auto emitWhile(const vdlisp::Value &rest) -> llvm::Value *;
    auto emitSet(const vdlisp::Value &rest) -> llvm::Value *;
    auto emitGenericForm(const std::string &opname, const vdlisp::Value &rest) -> llvm::Value *;
    auto emitFuncCall(const std::string &opname, const std::vector<llvm::Value *> &vals) -> llvm::Value *;
};

#endif // JIT_JIT_IR_EMITTER_HPP
