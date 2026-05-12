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

using namespace vdlisp;
using namespace llvm;

// emitExpr 对不支持的语法形式直接返回 nullptr，build_func_ir 自然回退到 interpreter stub，
// 因此无需额外的预检 —— 上面的早期检查是冗余的。

static auto build_interpreter_stub(vdlisp::FuncData *func, llvm::Module &M,
                                   llvm::LLVMContext &context,
                                   const std::string &name) -> llvm::Function * {
  auto *dblPtr = llvm::PointerType::getUnqual(llvm::Type::getDoubleTy(context));
  auto *ft = llvm::FunctionType::get(llvm::Type::getDoubleTy(context), {dblPtr, llvm::Type::getInt32Ty(context)}, false);
  auto *F = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, name, &M);
  auto *BB = llvm::BasicBlock::Create(context, "entry", F);
  llvm::IRBuilder<> ir(BB);

  auto *i8ptr = llvm::PointerType::getUnqual(llvm::Type::getInt8Ty(context));
  auto *bridgeFt = llvm::FunctionType::get(llvm::Type::getDoubleTy(context),
      {i8ptr, llvm::PointerType::getUnqual(llvm::Type::getDoubleTy(context)), llvm::Type::getInt32Ty(context)}, false);
  auto bridge = M.getOrInsertFunction("VDLISP__call_interpreted_from_jit", bridgeFt);

  auto arg = F->arg_begin();
  auto *fdPtr = llvm::ConstantExpr::getIntToPtr(
      llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), reinterpret_cast<uintptr_t>(func)), i8ptr);
  ir.CreateRet(ir.CreateCall(bridge, {fdPtr, &*arg++, &*arg}));
  return F;
}

auto build_func_ir(vdlisp::FuncData *func, llvm::Module &M,
                   llvm::LLVMContext &context, const std::string &name)
    -> llvm::Function * {
  if (!func) return nullptr;

  auto *dblPtr = llvm::PointerType::getUnqual(llvm::Type::getDoubleTy(context));
  auto *ft = llvm::FunctionType::get(llvm::Type::getDoubleTy(context), {dblPtr, llvm::Type::getInt32Ty(context)}, false);
  auto *F = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, name, &M);

  BasicBlock *BB = BasicBlock::Create(context, "entry", F);
  IRBuilder<> entry_ir(BB);

  JITIREmitter emitter(func, F, context);

  vdlisp::Value body = func->body;
  llvm::Value *lastv = nullptr;
  // 顺序发射函数体，最终返回最后一个表达式的数值结果。
  while (body) {
    vdlisp::Value car = pair_car(body);
    llvm::Value *v = emitter.emitExpr(car);
    if (v == nullptr) {
      F->eraseFromParent();
      return build_interpreter_stub(func, M, context, name);
    }
    lastv = v;
    body = pair_cdr(body);
  }
  if (lastv == nullptr) {
    lastv = ConstantFP::get(llvm::Type::getDoubleTy(context), 0.0);
  }
  std::pair<llvm::BasicBlock *, llvm::Value *> to_ret{nullptr, lastv};
  for (auto &block : *F) {
    if (!block.getTerminator()) { to_ret = {&block, lastv}; break; }
  }
  if (!to_ret.first) to_ret.first = llvm::BasicBlock::Create(context, "return", F);
  llvm::IRBuilder<>(to_ret.first).CreateRet(to_ret.second);
  return emitter.finalize();
}
