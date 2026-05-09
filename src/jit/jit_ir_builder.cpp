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

static auto contains_unsupported_jit_form(const vdlisp::Value &expr) -> bool {
  if (!expr || expr.get_type() != vdlisp::TPAIR) return false;
  auto op = pair_car(expr);
  if (!op || op.get_type() != vdlisp::TSYMBOL) return true;
  for (auto walk = pair_cdr(expr); walk; walk = pair_cdr(walk)) {
    if (contains_unsupported_jit_form(pair_car(walk))) return true;
  }
  return false;
}

auto can_attempt_jit_compile(vdlisp::FuncData *func) -> bool {
  if (!func) return false;
  for (auto body = func->body; body; body = pair_cdr(body)) {
    if (contains_unsupported_jit_form(pair_car(body))) return false;
  }
  return true;
}

static auto make_ft(llvm::LLVMContext &ctx) -> llvm::FunctionType * {
  auto *dblPtr = llvm::PointerType::getUnqual(llvm::Type::getDoubleTy(ctx));
  return llvm::FunctionType::get(llvm::Type::getDoubleTy(ctx), {dblPtr, llvm::Type::getInt32Ty(ctx)}, false);
}

static auto build_interpreter_stub(vdlisp::FuncData *func, llvm::Module &M,
                                   llvm::LLVMContext &context,
                                   const std::string &name) -> llvm::Function * {
  auto *F = llvm::Function::Create(make_ft(context), llvm::Function::ExternalLinkage, name, &M);
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
  if (!can_attempt_jit_compile(func))
    return build_interpreter_stub(func, M, context, name);

  auto *F = llvm::Function::Create(make_ft(context), llvm::Function::ExternalLinkage, name, &M);

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
