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

JITIREmitter::JITIREmitter(vdlisp::FuncData *func_, llvm::Function *F_, llvm::LLVMContext &context_)
    : func(func_), F(F_), context(context_), ir(&F_->getEntryBlock()) {
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
    // 所有可变局部都统一放在入口块 alloca，避免在控制流中重复分配。
    auto it = locals.find(name);
    if (it != locals.end())
        return it->second;
    llvm::IRBuilder<> tmp(&F->getEntryBlock(), F->getEntryBlock().begin());
    llvm::AllocaInst *a = tmp.CreateAlloca(llvm::Type::getDoubleTy(context));
    locals[name] = a;
    return a;
}

auto JITIREmitter::emitExpr(const vdlisp::Value &expr) -> llvm::Value * {
    // JIT 只处理“数值子语言”，所有表达式最终都必须落成 double。
    if (!expr)
        return llvm::ConstantFP::get(llvm::Type::getDoubleTy(context), 0.0);
    if (expr.get_type() == vdlisp::TNUMBER) {
        return llvm::ConstantFP::get(llvm::Type::getDoubleTy(context), expr.get_number());
    }
    if (expr.get_type() == vdlisp::TSYMBOL) {
        // 在数值 JIT 里，truthy 统一编码成 1.0，nil/false 编码成 0.0。
        if (*expr.get_symbol() == "#t") {
            return llvm::ConstantFP::get(llvm::Type::getDoubleTy(context), 1.0);
        }
        auto it = param_index.find(*expr.get_symbol());
        if (it != param_index.end()) {
            int i = it->second;
            llvm::Value *argptr = F->getArg(0);
            llvm::Value *idxv = llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), i);
            llvm::Value *gep = ir.CreateInBoundsGEP(llvm::Type::getDoubleTy(context), argptr, {idxv});
            return ir.CreateLoad(llvm::Type::getDoubleTy(context), gep);
        }
        auto lit = locals.find(*expr.get_symbol());
        if (lit != locals.end()) {
            return ir.CreateLoad(llvm::Type::getDoubleTy(context), lit->second);
        }

        // 自由变量无法静态解析时，生成一次运行时查找。
        llvm::Module *M = F->getParent();
        llvm::Type *dblTy = llvm::Type::getDoubleTy(context);
        llvm::Type *i8ptr = llvm::PointerType::getUnqual(llvm::Type::getInt8Ty(context));
        llvm::FunctionType *ft = llvm::FunctionType::get(dblTy, {i8ptr, i8ptr}, false);
        llvm::FunctionCallee callee = M->getOrInsertFunction("VDLISP__jit_lookup_number", ft);

        auto env_ptr_ty = i8ptr;
        auto env_bits_ty = llvm::Type::getInt64Ty(context);
        uintptr_t env_addr = reinterpret_cast<uintptr_t>(func && func->closure_env ? func->closure_env : nullptr);
        llvm::Constant *env_int = llvm::ConstantInt::get(env_bits_ty, static_cast<uint64_t>(env_addr));
        llvm::Constant *env_ptr = llvm::ConstantExpr::getIntToPtr(env_int, env_ptr_ty);

        llvm::Value *name_ptr = ir.CreateGlobalStringPtr(*expr.get_symbol());
        return ir.CreateCall(callee, {env_ptr, name_ptr});
    }
    if (expr.get_type() == vdlisp::TPAIR) {
        vdlisp::PairData *pd = expr.get_pair();
        vdlisp::Value op = pd->car;
        vdlisp::Value rest = pd->cdr;
        if (!op || op.get_type() != vdlisp::TSYMBOL)
            return nullptr;
        std::string opname = *op.get_symbol();

        if (opname == "let") {
            // 支持两种 let 绑定写法：扁平表和列表列表。
            if (!rest || rest.get_type() != TPAIR)
                return nullptr;
            vdlisp::Value bindings = pair_car(rest);
            vdlisp::Value body = pair_cdr(rest);

            // 先计算右值，再统一写入，避免后续绑定意外影响前面的求值。
            std::vector<std::pair<std::string, llvm::Value *>> evaluated;
            
            if (bindings.get_type() == TSYMBOL) {
                vdlisp::Value walk = bindings;
            }
            
            // 若第一项就是 symbol，则按 (let (a 1 b 2) ...) 的扁平格式解释。
            if (bindings.get_type() == TPAIR && pair_car(bindings).get_type() == TSYMBOL) {
                vdlisp::Value walk = bindings;
                while (walk) {
                    vdlisp::Value var = pair_car(walk);
                    if (var.get_type() != TSYMBOL) break;
                    walk = pair_cdr(walk);
                    if (!walk) break;
                    vdlisp::Value val_expr = pair_car(walk);
                    llvm::Value *v = emitExpr(val_expr);
                    if (!v) return nullptr;
                    evaluated.push_back({*var.get_symbol(), v});
                    walk = pair_cdr(walk);
                }
            } else {
                // List of lists format: (let ((a 1) (b 2)) body)
                for (vdlisp::Value b = bindings; b; b = pair_cdr(b)) {
                    vdlisp::Value entry = pair_car(b);
                    if (!entry || entry.get_type() != TPAIR)
                        return nullptr;
                    vdlisp::Value var = pair_car(entry);
                    if (var.get_type() != TSYMBOL)
                        return nullptr;
                    vdlisp::Value val_expr = pair_car(pair_cdr(entry));
                    llvm::Value *v = emitExpr(val_expr);
                    if (!v)
                        return nullptr;
                    evaluated.push_back({*var.get_symbol(), v});
                }
            }

            // 第二遍再写局部槽位，模拟解释器里 let 的批量绑定行为。
            for (auto &pair : evaluated) {
                ir.CreateStore(pair.second, ensure_local(pair.first));
            }

            llvm::Value *last = nullptr;
            for (vdlisp::Value b = body; b; b = pair_cdr(b)) {
                last = emitExpr(pair_car(b));
                if (!last)
                    return nullptr;
            }
            return last ? last : llvm::ConstantFP::get(llvm::Type::getDoubleTy(context), 0.0);
        }

        if (opname == "cond") {
            // `cond` 会展开成多个分支块，最终在 continuation 上汇合成一个 PHI。
            llvm::BasicBlock *contBB = llvm::BasicBlock::Create(context, "cond_cont", F);
            llvm::PHINode *phi = nullptr;

            for (vdlisp::Value c = rest; c; c = pair_cdr(c)) {
                vdlisp::Value clause = pair_car(c);
                if (!clause || clause.get_type() != TPAIR)
                    return nullptr;
                vdlisp::Value test = pair_car(clause);
                vdlisp::Value body = pair_cdr(clause);

                llvm::Value *testV = emitExpr(test);
                if (!testV)
                    return nullptr;

                llvm::Value *isTrue = ir.CreateFCmpUNE(testV, llvm::ConstantFP::get(llvm::Type::getDoubleTy(context), 0.0));
                
                llvm::BasicBlock *thenBB = llvm::BasicBlock::Create(context, "cond_then", F);
                llvm::BasicBlock *nextBB = llvm::BasicBlock::Create(context, "cond_next", F);

                ir.CreateCondBr(isTrue, thenBB, nextBB);

                ir.SetInsertPoint(thenBB);
                llvm::Value *last = nullptr;
                if (!body) {
                    last = testV;
                } else {
                    for (vdlisp::Value b = body; b; b = pair_cdr(b)) {
                        last = emitExpr(pair_car(b));
                        if (!last)
                            return nullptr;
                    }
                }
                if (!last)
                    last = llvm::ConstantFP::get(llvm::Type::getDoubleTy(context), 0.0);
                
                if (!ir.GetInsertBlock()->getTerminator()) {
                    ir.CreateBr(contBB);
                    if (!phi) {
                        llvm::IRBuilder<> tmp(contBB);
                        phi = tmp.CreatePHI(llvm::Type::getDoubleTy(context), 0);
                    }
                    phi->addIncoming(last, ir.GetInsertBlock());
                }
                
                ir.SetInsertPoint(nextBB);
            }

            // 所有分支都不命中时，结果按 nil 的数值编码 0.0 处理。
            if (!ir.GetInsertBlock()->getTerminator()) {
                ir.CreateBr(contBB);
                if (!phi) {
                    llvm::IRBuilder<> tmp(contBB);
                    phi = tmp.CreatePHI(llvm::Type::getDoubleTy(context), 0);
                }
                phi->addIncoming(llvm::ConstantFP::get(llvm::Type::getDoubleTy(context), 0.0), ir.GetInsertBlock());
            }

            ir.SetInsertPoint(contBB);
            return phi;
        }

        if (opname == "while") {
            // `while` 直接翻成 test -> loop -> after 三段 basic block。
            if (!rest || rest.get_type() != TPAIR)
                return nullptr;
            vdlisp::Value test = pair_car(rest);
            vdlisp::Value body = pair_cdr(rest);

            llvm::BasicBlock *testBB = llvm::BasicBlock::Create(context, "while_test", F);
            llvm::BasicBlock *loopBB = llvm::BasicBlock::Create(context, "while_loop", F);
            llvm::BasicBlock *afterBB = llvm::BasicBlock::Create(context, "while_after", F);

            ir.CreateBr(testBB);

            ir.SetInsertPoint(testBB);
            llvm::Value *testV = emitExpr(test);
            if (!testV)
                return nullptr;
            llvm::Value *isTrue = ir.CreateFCmpUNE(testV, llvm::ConstantFP::get(llvm::Type::getDoubleTy(context), 0.0));
            ir.CreateCondBr(isTrue, loopBB, afterBB);

            ir.SetInsertPoint(loopBB);
            for (vdlisp::Value b = body; b; b = pair_cdr(b)) {
                if (!emitExpr(pair_car(b)))
                    return nullptr;
            }
            ir.CreateBr(testBB);

            ir.SetInsertPoint(afterBB);
            return llvm::ConstantFP::get(llvm::Type::getDoubleTy(context), 0.0);
        }

        if (opname == "set"/* || opname == "set!" */) {
            // JIT 中的 set 只支持写局部变量或形参的局部镜像。
            if (!rest || rest.get_type() != TPAIR)
                return nullptr;
            vdlisp::Value var = pair_car(rest);
            if (var.get_type() != TSYMBOL)
                return nullptr;
            vdlisp::Value val_expr = pair_car(pair_cdr(rest));
            llvm::Value *valV = emitExpr(val_expr);
            if (!valV)
                return nullptr;

            std::string name = *var.get_symbol();
            auto it = locals.find(name);
            if (it != locals.end()) {
                ir.CreateStore(valV, it->second);
                return valV;
            }

            auto pit = param_index.find(name);
            if (pit != param_index.end()) {
                ir.CreateStore(valV, ensure_local(name));
                return valV;
            }

            return nullptr;
        }

        std::vector<llvm::Value *> vals;
        vdlisp::Value a = rest;
        while (a) {
            vdlisp::Value av = pair_car(a);
            llvm::Value *v = emitExpr(av);
            if (!v)
                return nullptr;
            vals.push_back(v);
            a = a.get_pair()->cdr;
        }
        if (opname == "+") {
            if (vals.size() != 2)
                return nullptr;
            return ir.CreateFAdd(vals[0], vals[1]);
        } else if (opname == "*") {
            if (vals.size() != 2)
                return nullptr;
            return ir.CreateFMul(vals[0], vals[1]);
        } else if (opname == "-") {
            if (vals.size() != 2)
                return nullptr;
            return ir.CreateFSub(vals[0], vals[1]);
        } else if (opname == "/") {
            if (vals.size() != 2)
                return nullptr;
            return ir.CreateFDiv(vals[0], vals[1]);
        }

        if (opname == "<" || opname == ">" || opname == "<=" || opname == ">=" || opname == "=") {
            if (vals.size() != 2)
                return nullptr;
            llvm::Value *L = vals[0];
            llvm::Value *R = vals[1];
            llvm::Value *cmp = nullptr;
            if (opname == "<")
                cmp = ir.CreateFCmpOLT(L, R);
            if (opname == ">")
                cmp = ir.CreateFCmpOGT(L, R);
            if (opname == "<=")
                cmp = ir.CreateFCmpOLE(L, R);
            if (opname == ">=")
                cmp = ir.CreateFCmpOGE(L, R);
            if (opname == "=")
                cmp = ir.CreateFCmpOEQ(L, R);
            return ir.CreateSelect(cmp, llvm::ConstantFP::get(llvm::Type::getDoubleTy(context), 1.0), llvm::ConstantFP::get(llvm::Type::getDoubleTy(context), 0.0));
        } // 当前 JIT 只支持二元算术/比较。
        const std::string *nm_ptr = op.get_symbol();
        Env *e = func->closure_env;
        if (e)
            retain_env(e);
        vdlisp::Value found;
        while (e) {
            auto it = e->map.find(*nm_ptr);
            if (it != e->map.end()) {
                found = it->second;
                break;
            }
            Env *next = e->parent;
            if (next)
                retain_env(next);
            release_env(e);
            e = next;
        }
        if (e)
            release_env(e);
        if (found && found.get_type() == vdlisp::TFUNC) {
            // 已知 callee 是闭包函数时，优先直接调用其机器码，否则退回 bridge。
            vdlisp::FuncData *callee_fd = found.get_func();
            if (!callee_fd)
                return nullptr;
            std::string callee_name = "jit_fn_" + std::to_string(reinterpret_cast<uintptr_t>(callee_fd));
            llvm::Module *M = F->getParent();
            llvm::Type *dblTy = llvm::Type::getDoubleTy(context);
            llvm::Type *dblPtr = llvm::PointerType::getUnqual(dblTy);
            llvm::FunctionType *native_ft = llvm::FunctionType::get(dblTy, {dblPtr, llvm::Type::getInt32Ty(context)}, false);

            llvm::Value *argArrayPtr = nullptr;
            if (vals.empty()) {
                argArrayPtr = llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(dblTy));
            } else {
                llvm::IRBuilder<> tmp(&F->getEntryBlock(), F->getEntryBlock().begin());
                llvm::Value *arrSize = llvm::ConstantInt::get(llvm::Type::getInt32Ty(context), (int)vals.size());
                llvm::AllocaInst *all = tmp.CreateAlloca(dblTy, arrSize);
                for (int i = 0; i < (int)vals.size(); ++i) {
                    llvm::Value *idx = llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), i);
                    llvm::Value *gep = ir.CreateInBoundsGEP(dblTy, all, {idx});
                    ir.CreateStore(vals[i], gep);
                }
                argArrayPtr = all;
            }
            llvm::Value *argcV = llvm::ConstantInt::get(llvm::Type::getInt32Ty(context), (int)vals.size());

            if (callee_fd->compiled_code) {
                llvm::FunctionCallee fc = M->getOrInsertFunction(callee_name, native_ft);
                llvm::Value *callv = ir.CreateCall(fc, {argArrayPtr, argcV});
                return callv;
            }

            llvm::Type *i8ptr = llvm::PointerType::getUnqual(llvm::Type::getInt8Ty(context));
            llvm::FunctionType *bridge_ft = llvm::FunctionType::get(dblTy, {i8ptr, dblPtr, llvm::Type::getInt32Ty(context)}, false);
            llvm::FunctionCallee bridge = M->getOrInsertFunction("VDLISP__call_from_jit", bridge_ft);
            llvm::Constant *fd_c = llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), (uint64_t)callee_fd);
            llvm::Constant *fd_ptr = llvm::ConstantExpr::getIntToPtr(fd_c, i8ptr);
            llvm::Value *callv = ir.CreateCall(bridge, {fd_ptr, argArrayPtr, argcV});
            return callv;
        }

        return nullptr;
    }
    return nullptr;
}

auto JITIREmitter::finalize() -> llvm::Function * {
    return F;
}
