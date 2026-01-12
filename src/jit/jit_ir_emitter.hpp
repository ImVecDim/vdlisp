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
class PairData;
} // namespace vdlisp

class JITIREmitter {
  public:
    JITIREmitter(vdlisp::FuncData *func, llvm::Function *F, llvm::LLVMContext &context);
    auto emitExpr(const vdlisp::Value &expr) -> llvm::Value *;
    auto compileCond(const vdlisp::Value &clauses) -> llvm::Value *;
    auto compileWhile(const vdlisp::Value &rest) -> llvm::Value *;
    auto compileLet(const vdlisp::Value &rest) -> llvm::Value *;
    auto finalize() -> llvm::Function *;

  private:
    vdlisp::FuncData *func;
    llvm::Function *F;
    llvm::LLVMContext &context;
    llvm::IRBuilder<> ir;
    std::unordered_map<std::string, llvm::AllocaInst *> locals;
    std::unordered_map<std::string, int> param_index;

    auto ensure_local(const std::string &name) -> llvm::AllocaInst *;
};

#endif // JIT_JIT_IR_EMITTER_HPP
