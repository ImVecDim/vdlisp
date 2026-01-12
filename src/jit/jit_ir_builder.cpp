// Separated IR builder for JIT compilation.
#include "jit/jit_ir_builder.hpp"
#include "helpers.hpp"
#include "nanbox.hpp"

#include "jit/jit_ir_emitter.hpp"
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Verifier.h>
#include <string>
#include <unordered_map>
#include <vector>

using namespace vdlisp;
using namespace llvm;

auto build_func_ir(vdlisp::FuncData *func, llvm::Module &M, llvm::LLVMContext &context, const std::string &name) -> llvm::Function * {
    if (!func)
        return nullptr;

    std::vector<llvm::Type *> fparams = {llvm::PointerType::getUnqual(llvm::Type::getDoubleTy(context)), llvm::Type::getInt32Ty(context)};
    FunctionType *ft = FunctionType::get(llvm::Type::getDoubleTy(context), llvm::ArrayRef<llvm::Type *>(fparams.data(), fparams.size()), false);
    Function *F = Function::Create(ft, Function::ExternalLinkage, name, &M);

    BasicBlock *BB = BasicBlock::Create(context, "entry", F);
    IRBuilder<> entry_ir(BB);

    JITIREmitter emitter(func, F, context);

    vdlisp::Value body = func->body;
    llvm::Value *lastv = nullptr;
    while (body) {
        vdlisp::Value car = pair_car(body);
        llvm::Value *v = emitter.emitExpr(car);
        if (!v)
            return nullptr;
        lastv = v;
        body = pair_cdr(body);
    }
    if (!lastv)
        lastv = ConstantFP::get(llvm::Type::getDoubleTy(context), 0.0);
    // If the value was produced in a block that does not yet have a
    // terminator (e.g. the cond continuation block), emit the return
    // there to avoid inserting a return into the entry block after
    // a previously-created branch (which makes invalid IR).
    if (auto inst = llvm::dyn_cast<llvm::Instruction>(lastv)) {
        if (llvm::BasicBlock *parent = inst->getParent()) {
            if (!parent->getTerminator()) {
                llvm::IRBuilder<> bb_ir(parent);
                bb_ir.CreateRet(lastv);
                return emitter.finalize();
            }
        }
    }
    entry_ir.CreateRet(lastv);
    return emitter.finalize();
}
