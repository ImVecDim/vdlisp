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
    auto it = locals.find(name);
    if (it != locals.end())
        return it->second;
    llvm::IRBuilder<> tmp(&F->getEntryBlock(), F->getEntryBlock().begin());
    llvm::AllocaInst *a = tmp.CreateAlloca(llvm::Type::getDoubleTy(context));
    locals[name] = a;
    return a;
}

auto JITIREmitter::compileCond(const vdlisp::Value &clauses) -> llvm::Value * {
    if (!clauses)
        return llvm::ConstantFP::get(llvm::Type::getDoubleTy(context), 0.0);
    llvm::BasicBlock *contBB = llvm::BasicBlock::Create(context, "cond_cont", F);
    std::vector<std::pair<llvm::Value *, llvm::BasicBlock *>> incoming;

    vdlisp::Value walk = clauses;
    int idx = 0;

    // Handle empty condition case if needed, though usually walk is not null if called correctly
    if (!walk) {
        contBB->eraseFromParent();
        return llvm::ConstantFP::get(llvm::Type::getDoubleTy(context), 0.0);
    }

    while (walk) {
        vdlisp::Value clause = pair_car(walk);
        vdlisp::Value test = (is_pair(clause)) ? pair_car(clause) : vdlisp::Value();
        vdlisp::Value body = (is_pair(clause)) ? pair_cdr(clause) : vdlisp::Value();

        // Emit test
        llvm::Value *condv = emitExpr(test);
        if (!condv)
            return nullptr;

        llvm::Value *zero = llvm::ConstantFP::get(llvm::Type::getDoubleTy(context), 0.0);
        llvm::Value *is_true = ir.CreateFCmpONE(condv, zero);

        llvm::BasicBlock *bodyBB = llvm::BasicBlock::Create(context, "cond_body" + std::to_string(idx), F);
        llvm::BasicBlock *nextBB = llvm::BasicBlock::Create(context, "cond_next" + std::to_string(idx), F);

        ir.CreateCondBr(is_true, bodyBB, nextBB);

        // Emit body
        ir.SetInsertPoint(bodyBB);
        llvm::Value *last = nullptr;
        while (body) {
            vdlisp::Value ex = pair_car(body);
            llvm::Value *v = emitExpr(ex);
            if (!v)
                return nullptr;
            last = v;
            body = body.get_pair()->cdr;
        }
        if (!last)
            last = llvm::ConstantFP::get(llvm::Type::getDoubleTy(context), 0.0);

        ir.CreateBr(contBB);
        incoming.push_back({last, ir.GetInsertBlock()});

        // Move to next
        ir.SetInsertPoint(nextBB);
        walk = walk.get_pair()->cdr;
        ++idx;
    }

    // Fallthrough case
    llvm::Value *defVal = llvm::ConstantFP::get(llvm::Type::getDoubleTy(context), 0.0);
    ir.CreateBr(contBB);
    incoming.push_back({defVal, ir.GetInsertBlock()});

    ir.SetInsertPoint(contBB);
    llvm::PHINode *phi = ir.CreatePHI(llvm::Type::getDoubleTy(context), (unsigned)incoming.size());
    for (auto &p : incoming)
        phi->addIncoming(p.first, p.second);

    return phi;
}
auto JITIREmitter::compileWhile(const vdlisp::Value &rest) -> llvm::Value * {
    vdlisp::Value cond = pair_car(rest);
    vdlisp::Value body = rest.get_pair()->cdr;
    llvm::Value *zero = llvm::ConstantFP::get(llvm::Type::getDoubleTy(context), 0.0);

    llvm::BasicBlock *loopBB = llvm::BasicBlock::Create(context, "loop", F);
    llvm::BasicBlock *bodyBB = llvm::BasicBlock::Create(context, "loopbody", F);
    llvm::BasicBlock *contBB = llvm::BasicBlock::Create(context, "loopcont", F);

    ir.CreateBr(loopBB);
    ir.SetInsertPoint(loopBB);
    llvm::Value *condv = emitExpr(cond);
    if (!condv)
        return nullptr;
    llvm::Value *is_true = ir.CreateFCmpONE(condv, zero);
    ir.CreateCondBr(is_true, bodyBB, contBB);

    ir.SetInsertPoint(bodyBB);
    llvm::Value *last = nullptr;
    vdlisp::Value bb = body;
    while (bb) {
        vdlisp::Value ex = pair_car(bb);
        llvm::Value *v = emitExpr(ex);
        if (!v)
            return nullptr;
        last = v;
        bb = bb.get_pair()->cdr;
    }
    if (!last)
        last = llvm::ConstantFP::get(llvm::Type::getDoubleTy(context), 0.0);
    ir.CreateBr(loopBB);

    ir.SetInsertPoint(contBB);
    return last;
}

auto JITIREmitter::compileLet(const vdlisp::Value &rest) -> llvm::Value * {
    vdlisp::Value bindings = pair_car(rest);
    vdlisp::Value letbody = rest.get_pair()->cdr;
    vdlisp::Value b = bindings;
    if (is_pair(b) && is_pair(pair_car(b))) {
        while (b) {
            vdlisp::Value pair = pair_car(b);
            vdlisp::Value name = pair_car(pair);
            vdlisp::Value val = pair_car(pair_cdr(pair));
            if (!name || name.get_type() != vdlisp::TSYMBOL)
                return nullptr;
            llvm::Value *v = emitExpr(val);
            if (!v)
                return nullptr;
            llvm::AllocaInst *a = ensure_local(*name.get_symbol());
            ir.CreateStore(v, a);
            b = pair_cdr(b);
        }
    } else {
        while (b) {
            vdlisp::Value name = pair_car(b);
            if (!name || name.get_type() != vdlisp::TSYMBOL)
                return nullptr;
            vdlisp::Value next = pair_cdr(b);
            if (!next)
                return nullptr;
            vdlisp::Value val = pair_car(next);
            llvm::Value *v = emitExpr(val);
            if (!v)
                return nullptr;
            llvm::AllocaInst *a = ensure_local(*name.get_symbol());
            ir.CreateStore(v, a);
            b = pair_cdr(next);
        }
    }
    llvm::Value *last = nullptr;
    vdlisp::Value bb = letbody;
    while (bb) {
        vdlisp::Value ex = pair_car(bb);
        llvm::Value *v = emitExpr(ex);
        if (!v)
            return nullptr;
        last = v;
        bb = pair_cdr(bb);
    }
    if (!last)
        last = llvm::ConstantFP::get(llvm::Type::getDoubleTy(context), 0.0);
    return last;
}
auto JITIREmitter::emitExpr(const vdlisp::Value &expr) -> llvm::Value * {
    if (!expr)
        return llvm::ConstantFP::get(llvm::Type::getDoubleTy(context), 0.0);
    if (expr.get_type() == vdlisp::TNUMBER) {
        return llvm::ConstantFP::get(llvm::Type::getDoubleTy(context), expr.get_number());
    }
    if (expr.get_type() == vdlisp::TSYMBOL) {
        // Builtin truthy literal: in the interpreter '#t' is a globally-bound symbol
        // (non-nil), but for the JIT's numeric representation we treat it as 1.0.
        // This avoids an environment lookup and allows cond/while default branches.
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

        // Free variable: try runtime lookup from closure env chain.
        // Returns NaN if unbound or non-numeric; the caller will then fall back.
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

        if (opname == "cond")
            return compileCond(rest);
        if (opname == "while")
            return compileWhile(rest);
        if (opname == "let")
            return compileLet(rest);

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
        } // TODO >2 vals???
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
