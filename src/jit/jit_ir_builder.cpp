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

// 只要遇到当前发射器还不会处理的结构，就不要冒险生成半吊子的机器码。
static auto contains_unsupported_jit_form(const vdlisp::Value &expr) -> bool {
    if (!expr)
        return false;
    if (expr.get_type() != vdlisp::TPAIR)
        return false;

    vdlisp::Value op = pair_car(expr);
    if (!op || op.get_type() != vdlisp::TSYMBOL)
        return true;

    // cond, while, let are now supported by the emitter
    for (vdlisp::Value walk = pair_cdr(expr); walk; walk = pair_cdr(walk)) {
        if (contains_unsupported_jit_form(pair_car(walk)))
            return true;
    }
    return false;
}

auto can_attempt_jit_compile(vdlisp::FuncData *func) -> bool {
    // 函数体里每个顶层表达式都必须能被当前 JIT 子集接受。
    if (!func)
        return false;
    for (vdlisp::Value body = func->body; body; body = pair_cdr(body)) {
        if (contains_unsupported_jit_form(pair_car(body)))
            return false;
    }
    return true;
}

static auto build_interpreter_stub(vdlisp::FuncData *func, llvm::Module &M, llvm::LLVMContext &context, const std::string &name) -> llvm::Function * {
    // 保底桩函数：签名保持一致，但内部直接桥接回解释器。
    std::vector<llvm::Type *> fparams = {llvm::PointerType::getUnqual(llvm::Type::getDoubleTy(context)), llvm::Type::getInt32Ty(context)};
    FunctionType *ft = FunctionType::get(llvm::Type::getDoubleTy(context), llvm::ArrayRef<llvm::Type *>(fparams.data(), fparams.size()), false);
    Function *F = Function::Create(ft, Function::ExternalLinkage, name, &M);

    BasicBlock *BB = BasicBlock::Create(context, "entry", F);
    IRBuilder<> ir(BB);
    llvm::Type *dblTy = llvm::Type::getDoubleTy(context);
    llvm::Type *dblPtr = llvm::PointerType::getUnqual(dblTy);
    llvm::Type *i8ptr = llvm::PointerType::getUnqual(llvm::Type::getInt8Ty(context));
    llvm::FunctionType *bridgeFt = llvm::FunctionType::get(dblTy, {i8ptr, dblPtr, llvm::Type::getInt32Ty(context)}, false);
    llvm::FunctionCallee bridge = M.getOrInsertFunction("VDLISP__call_interpreted_from_jit", bridgeFt);

    auto it = F->arg_begin();
    llvm::Value *argvPtr = &*it++;
    llvm::Value *argc = &*it;
    llvm::Constant *fdBits = llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), static_cast<uint64_t>(reinterpret_cast<uintptr_t>(func)));
    llvm::Constant *fdPtr = llvm::ConstantExpr::getIntToPtr(fdBits, i8ptr);
    llvm::Value *res = ir.CreateCall(bridge, {fdPtr, argvPtr, argc});
    ir.CreateRet(res);
    return F;
}

auto build_func_ir(vdlisp::FuncData *func, llvm::Module &M, llvm::LLVMContext &context, const std::string &name) -> llvm::Function * {
    if (!func)
        return nullptr;
    if (!can_attempt_jit_compile(func))
        return build_interpreter_stub(func, M, context, name);

    std::vector<llvm::Type *> fparams = {llvm::PointerType::getUnqual(llvm::Type::getDoubleTy(context)), llvm::Type::getInt32Ty(context)};
    FunctionType *ft = FunctionType::get(llvm::Type::getDoubleTy(context), llvm::ArrayRef<llvm::Type *>(fparams.data(), fparams.size()), false);
    Function *F = Function::Create(ft, Function::ExternalLinkage, name, &M);

    BasicBlock *BB = BasicBlock::Create(context, "entry", F);
    IRBuilder<> entry_ir(BB);

    JITIREmitter emitter(func, F, context);

    vdlisp::Value body = func->body;
    llvm::Value *lastv = nullptr;
    // 顺序发射函数体，最终返回最后一个表达式的数值结果。
    while (body) {
        vdlisp::Value car = pair_car(body);
        llvm::Value *v = emitter.emitExpr(car);
        if (!v) {
            F->eraseFromParent();
            return build_interpreter_stub(func, M, context, name);
        }
        lastv = v;
        body = pair_cdr(body);
    }
    if (!lastv)
        lastv = ConstantFP::get(llvm::Type::getDoubleTy(context), 0.0);
    // 控制流表达式可能把最后一个值生成在别的 basic block 里；此时必须在那个 block 上补 ret。
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
