#ifndef JIT_JIT_IR_EMITTER_HPP
#define JIT_JIT_IR_EMITTER_HPP

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Function.h>
#include <string>
#include <unordered_map>
#include <functional>

namespace vdlisp { class FuncData; class Value; class PairData; template <class T> class sptr; using Ptr = sptr<Value>; }

class JITIREmitter {
public:
    JITIREmitter(vdlisp::FuncData* func, llvm::Function* F, llvm::LLVMContext &context);
    auto emitExpr(vdlisp::Ptr expr) -> llvm::Value*;
    auto compileCond(vdlisp::Ptr clauses) -> llvm::Value*;
    auto compileWhile(vdlisp::Ptr rest) -> llvm::Value*;
    auto compileLet(vdlisp::Ptr rest) -> llvm::Value*;
    auto finalize() -> llvm::Function*;

private:
    vdlisp::FuncData* func;
    llvm::Function* F;
    llvm::LLVMContext &context;
    llvm::IRBuilder<> ir;
    std::unordered_map<std::string, llvm::AllocaInst*> locals;
    std::unordered_map<std::string,int> param_index;

    auto ensure_local(const std::string &name) -> llvm::AllocaInst*;
};

#endif // JIT_JIT_IR_EMITTER_HPP
