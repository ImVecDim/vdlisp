// Implementation of a focused IR emitter used by build_func_ir.
#include "jit/jit_ir_emitter.hpp"
#include "helpers.hpp"
#include "nanbox.hpp"

#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>

using namespace vdlisp;
using namespace llvm;

JITIREmitter::JITIREmitter(vdlisp::FuncData *func_, llvm::Function *F_,
                           llvm::LLVMContext &context_)
    : func(func_), F(F_), context(context_), ir(&F_->getEntryBlock()),
      dblTy(llvm::Type::getDoubleTy(context_)),
      dblZero(llvm::ConstantFP::get(dblTy, 0.0)),
      dblOne(llvm::ConstantFP::get(dblTy, 1.0)) {
  // 先扫描形参，把符号名映射到 argv 数组下标。
  vdlisp::Value p = func->params;
  int idx = 0;
  while (p) {
    if (p.get_type() == TSYMBOL) {
      param_index[*p.get_symbol()] = idx++;
      break;
    }
    PairData *ppd = p.get_pair();
    vdlisp::Value pname = ppd->car;
    if (pname && pname.get_type() == TSYMBOL) {
      param_index[*pname.get_symbol()] = idx++;
    }
    p = ppd->cdr;
  }
}

auto JITIREmitter::ensure_local(const std::string &name) -> AllocaInst * {
  auto [it, inserted] = locals.try_emplace(name);
  if (inserted) {
    llvm::IRBuilder<> tmp(&F->getEntryBlock(), F->getEntryBlock().begin());
    it->second = tmp.CreateAlloca(dblTy);
  }
  return it->second;
}

// 按语法形式拆分的子发射器 ------------------------------------------------

auto JITIREmitter::emitSymbol(const vdlisp::Value &expr) -> llvm::Value * {
  // #t 编码为 1.0
  if (*expr.get_symbol() == "#t") return dblOne;

  // 形参：从 argv 数组读取
  {
    auto it = param_index.find(*expr.get_symbol());
    if (it != param_index.end()) {
      llvm::Value *idxv = ConstantInt::get(llvm::Type::getInt64Ty(context), it->second);
      llvm::Value *gep = ir.CreateInBoundsGEP(dblTy, F->getArg(0), {idxv});
      return ir.CreateLoad(dblTy, gep);
    }
  }

  // let 绑定的局部变量
  {
    auto lit = locals.find(*expr.get_symbol());
    if (lit != locals.end())
      return ir.CreateLoad(dblTy, lit->second);
  }

  // 自由变量：运行时桥接查找
  llvm::Module *M = F->getParent();
  llvm::Type *i8ptr = PointerType::getUnqual(llvm::Type::getInt8Ty(context));
  llvm::FunctionType *ft = FunctionType::get(dblTy, {i8ptr, i8ptr}, false);
  llvm::FunctionCallee callee = M->getOrInsertFunction("VDLISP__jit_lookup_number", ft);

  auto env_addr = reinterpret_cast<uintptr_t>(
      (func != nullptr) && (func->closure_env != nullptr) ? func->closure_env : nullptr);
  llvm::Constant *env_ptr = ConstantExpr::getIntToPtr(
      ConstantInt::get(llvm::Type::getInt64Ty(context), static_cast<uint64_t>(env_addr)), i8ptr);
  return ir.CreateCall(callee, {env_ptr, ir.CreateGlobalStringPtr(*expr.get_symbol())});
}

auto JITIREmitter::emitLet(const vdlisp::Value &rest) -> llvm::Value * {
  if (!rest || rest.get_type() != TPAIR) return nullptr;
  vdlisp::Value bindings = pair_car(rest);
  vdlisp::Value body = pair_cdr(rest);

  // 先计算右值再统一写入，避免绑定间相互影响
  std::vector<std::pair<std::string, llvm::Value *>> evaluated;

  if (bindings.get_type() == TPAIR && pair_car(bindings).get_type() == TSYMBOL) {
    // 扁平格式: (let (a 1 b 2) body)
    for (vdlisp::Value walk = bindings; walk; walk = pair_cdr(pair_cdr(walk))) {
      if (!walk || !pair_cdr(walk)) break;
      vdlisp::Value var = pair_car(walk);
      if (var.get_type() != TSYMBOL) break;
      llvm::Value *v = emitExpr(pair_car(pair_cdr(walk)));
      if (v == nullptr) return nullptr;
      evaluated.emplace_back(*var.get_symbol(), v);
    }
  } else {
    // 列表列表: (let ((a 1) (b 2)) body)
    for (vdlisp::Value b = bindings; b; b = pair_cdr(b)) {
      vdlisp::Value entry = pair_car(b);
      if (!entry || entry.get_type() != TPAIR) return nullptr;
      vdlisp::Value var = pair_car(entry);
      if (var.get_type() != TSYMBOL) return nullptr;
      llvm::Value *v = emitExpr(pair_car(pair_cdr(entry)));
      if (v == nullptr) return nullptr;
      evaluated.emplace_back(*var.get_symbol(), v);
    }
  }

  for (auto &pair : evaluated)
    ir.CreateStore(pair.second, ensure_local(pair.first));

  // 发射 body
  llvm::Value *last = nullptr;
  for (vdlisp::Value b = body; b; b = pair_cdr(b)) {
    last = emitExpr(pair_car(b));
    if (last == nullptr) return nullptr;
  }
  return last ? last : dblZero;
}

auto JITIREmitter::emitCond(const vdlisp::Value &rest) -> llvm::Value * {
  llvm::BasicBlock *contBB = BasicBlock::Create(context, "cond_cont", F);
  llvm::PHINode *phi = nullptr;

  for (vdlisp::Value c = rest; c; c = pair_cdr(c)) {
    vdlisp::Value clause = pair_car(c);
    if (!clause || clause.get_type() != TPAIR) return nullptr;
    vdlisp::Value test = pair_car(clause);
    vdlisp::Value body = pair_cdr(clause);

    llvm::Value *testV = emitExpr(test);
    if (testV == nullptr) return nullptr;

    llvm::Value *isTrue = ir.CreateFCmpUNE(testV, dblZero);
    llvm::BasicBlock *thenBB = BasicBlock::Create(context, "cond_then", F);
    llvm::BasicBlock *nextBB = BasicBlock::Create(context, "cond_next", F);
    ir.CreateCondBr(isTrue, thenBB, nextBB);

    ir.SetInsertPoint(thenBB);
    llvm::Value *last = nullptr;
    if (body) {
      for (vdlisp::Value b = body; b; b = pair_cdr(b)) {
        last = emitExpr(pair_car(b));
        if (last == nullptr) return nullptr;
      }
    }
    if (last == nullptr) last = testV;

    if (ir.GetInsertBlock()->getTerminator() == nullptr) {
      ir.CreateBr(contBB);
      if (phi == nullptr) { llvm::IRBuilder<> tmp(contBB); phi = tmp.CreatePHI(dblTy, 0); }
      phi->addIncoming(last, ir.GetInsertBlock());
    }
    ir.SetInsertPoint(nextBB);
  }

  // 全未命中时结果为 0.0
  if (ir.GetInsertBlock()->getTerminator() == nullptr) {
    ir.CreateBr(contBB);
    if (phi == nullptr) { llvm::IRBuilder<> tmp(contBB); phi = tmp.CreatePHI(dblTy, 0); }
    phi->addIncoming(dblZero, ir.GetInsertBlock());
  }

  ir.SetInsertPoint(contBB);
  return phi;
}

auto JITIREmitter::emitWhile(const vdlisp::Value &rest) -> llvm::Value * {
  if (!rest || rest.get_type() != TPAIR) return nullptr;
  vdlisp::Value test = pair_car(rest);
  vdlisp::Value body = pair_cdr(rest);

  llvm::BasicBlock *testBB = BasicBlock::Create(context, "while_test", F);
  llvm::BasicBlock *loopBB = BasicBlock::Create(context, "while_loop", F);
  llvm::BasicBlock *afterBB = BasicBlock::Create(context, "while_after", F);

  ir.CreateBr(testBB);
  ir.SetInsertPoint(testBB);

  llvm::Value *testV = emitExpr(test);
  if (testV == nullptr) return nullptr;
  ir.CreateCondBr(ir.CreateFCmpUNE(testV, dblZero), loopBB, afterBB);

  ir.SetInsertPoint(loopBB);
  for (vdlisp::Value b = body; b; b = pair_cdr(b)) {
    if (emitExpr(pair_car(b)) == nullptr) return nullptr;
  }
  ir.CreateBr(testBB);

  ir.SetInsertPoint(afterBB);
  return dblZero;
}

auto JITIREmitter::emitSet(const vdlisp::Value &rest) -> llvm::Value * {
  if (!rest || rest.get_type() != TPAIR) return nullptr;
  vdlisp::Value var = pair_car(rest);
  if (var.get_type() != TSYMBOL) return nullptr;

  llvm::Value *valV = emitExpr(pair_car(pair_cdr(rest)));
  if (valV == nullptr) return nullptr;

  std::string name = *var.get_symbol();

  // 局部变量写入
  auto it = locals.find(name);
  if (it != locals.end()) { ir.CreateStore(valV, it->second); return valV; }

  // 形参：同时写入 alloca 和 args 数组
  auto pit = param_index.find(name);
  if (pit != param_index.end()) {
    ir.CreateStore(valV, ensure_local(name));
    llvm::Value *idxv = ConstantInt::get(llvm::Type::getInt64Ty(context), pit->second);
    llvm::Value *gep = ir.CreateInBoundsGEP(dblTy, F->getArg(0), {idxv});
    ir.CreateStore(valV, gep);
    return valV;
  }

  return nullptr;
}

auto JITIREmitter::emitFuncCall(const std::string &opname,
                                 const std::vector<llvm::Value *> &vals) -> llvm::Value * {
  // 在闭包环境中查找被调用函数
  vdlisp::Value found;
  for (auto *e = func->closure_env; e; e = e->parent) {
    auto it = e->map.find(opname);
    if (it != e->map.end()) { found = it->second; break; }
  }
  if (!found || found.get_type() != vdlisp::TFUNC) return nullptr;

  vdlisp::FuncData *callee_fd = found.get_func();
  if (callee_fd == nullptr) return nullptr;

  llvm::Module *M = F->getParent();
  llvm::Type *dblPtr = PointerType::getUnqual(dblTy);
  llvm::FunctionType *native_ft =
      FunctionType::get(dblTy, {dblPtr, llvm::Type::getInt32Ty(context)}, false);

  // 构建实参数组 alloca
  llvm::Value *argArrayPtr = nullptr;
  if (vals.empty()) {
    argArrayPtr = ConstantPointerNull::get(PointerType::getUnqual(dblTy));
  } else {
    llvm::IRBuilder<> tmp(&F->getEntryBlock(), F->getEntryBlock().begin());
    llvm::AllocaInst *all = tmp.CreateAlloca(dblTy,
        ConstantInt::get(llvm::Type::getInt32Ty(context), static_cast<int>(vals.size())));
    for (int i = 0; i < static_cast<int>(vals.size()); ++i) {
      llvm::Value *gep = ir.CreateInBoundsGEP(dblTy, all,
          {ConstantInt::get(llvm::Type::getInt64Ty(context), i)});
      ir.CreateStore(vals[i], gep);
    }
    argArrayPtr = all;
  }
  llvm::Value *argcV = ConstantInt::get(llvm::Type::getInt32Ty(context),
                                         static_cast<int>(vals.size()));

  if (callee_fd->compiled_code != nullptr) {
    std::string callee_name = "jit_fn_" + std::to_string(reinterpret_cast<uintptr_t>(callee_fd));
    return ir.CreateCall(M->getOrInsertFunction(callee_name, native_ft), {argArrayPtr, argcV});
  }

  // 未编译时退回 bridge
  llvm::Type *i8ptr = PointerType::getUnqual(llvm::Type::getInt8Ty(context));
  llvm::FunctionType *bridge_ft =
      FunctionType::get(dblTy, {i8ptr, dblPtr, llvm::Type::getInt32Ty(context)}, false);
  llvm::Constant *fd_ptr = ConstantExpr::getIntToPtr(
      ConstantInt::get(llvm::Type::getInt64Ty(context), (uint64_t)callee_fd), i8ptr);
  return ir.CreateCall(M->getOrInsertFunction("VDLISP__call_from_jit", bridge_ft),
                        {fd_ptr, argArrayPtr, argcV});
}

auto JITIREmitter::emitGenericForm(const std::string &opname,
                                    const vdlisp::Value &rest) -> llvm::Value * {
  // 收集全部参数
  std::vector<llvm::Value *> vals;
  for (vdlisp::Value a = rest; a; a = a.get_pair()->cdr) {
    llvm::Value *v = emitExpr(pair_car(a));
    if (v == nullptr) return nullptr;
    vals.push_back(v);
  }
  if (vals.size() != 2) return nullptr;

  auto *L = vals[0], *R = vals[1];

  // 二元算术
  if (opname == "+") return ir.CreateFAdd(L, R);
  if (opname == "-") return ir.CreateFSub(L, R);
  if (opname == "*") return ir.CreateFMul(L, R);
  if (opname == "/") return ir.CreateFDiv(L, R);

  // 比较运算
  static const struct { const char *n; llvm::CmpInst::Predicate p; } cmp_tab[] = {
    {"<",  llvm::CmpInst::FCMP_OLT},
    {">",  llvm::CmpInst::FCMP_OGT},
    {"<=", llvm::CmpInst::FCMP_OLE},
    {">=", llvm::CmpInst::FCMP_OGE},
    {"=",  llvm::CmpInst::FCMP_OEQ},
  };
  for (auto &c : cmp_tab) {
    if (opname == c.n)
      return ir.CreateSelect(ir.CreateFCmp(c.p, L, R), dblOne, dblZero);
  }

  // 嵌套函数调用
  return emitFuncCall(opname, vals);
}

// 主发射入口 ---------------------------------------------------------------

auto JITIREmitter::emitExpr(const vdlisp::Value &expr) -> llvm::Value * {
  // 字面量：nil → 0.0，number → 常量
  if (!expr) return dblZero;
  if (expr.get_type() == vdlisp::TNUMBER)
    return llvm::ConstantFP::get(dblTy, expr.get_number());
  if (expr.get_type() == vdlisp::TSYMBOL)
    return emitSymbol(expr);
  if (expr.get_type() != vdlisp::TPAIR) return nullptr;

  // 调用/控制形式：根据操作符分发到各子发射器
  vdlisp::PairData *pd = expr.get_pair();
  vdlisp::Value op = pd->car;
  vdlisp::Value rest = pd->cdr;
  if (!op || op.get_type() != vdlisp::TSYMBOL) return nullptr;
  std::string opname = *op.get_symbol();

  if (opname == "let")   return emitLet(rest);
  if (opname == "cond")  return emitCond(rest);
  if (opname == "while") return emitWhile(rest);
  if (opname == "set")   return emitSet(rest);

  return emitGenericForm(opname, rest);
}

auto JITIREmitter::finalize() -> llvm::Function * { return F; }
