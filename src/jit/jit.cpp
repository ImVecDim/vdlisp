// jit.cpp (moved into src/jit)
#include "jit/jit.hpp"
#include <iostream>
#include <llvm/ExecutionEngine/ExecutionEngine.h>
#include <llvm/ExecutionEngine/MCJIT.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/TargetSelect.h>

#include "helpers.hpp"
#include "jit/jit_ir_builder.hpp"
#include "nanbox.hpp"
#include <unordered_map>

// Bridge declared in jit_bridge.cpp
extern "C" auto VDLISP__call_from_jit(void *, double *, int) noexcept -> double;

JITCompiler::JITCompiler() {
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    llvm::InitializeNativeTargetAsmParser();

    auto m = std::make_unique<llvm::Module>("jit_module", context);

    std::string error;
    executionEngine = std::unique_ptr<llvm::ExecutionEngine>(
        llvm::EngineBuilder(std::move(m))
            .setErrorStr(&error)
            .setEngineKind(llvm::EngineKind::JIT)
            .create());

    if (!executionEngine) {
        throw std::runtime_error("ExecutionEngine creation failed: " + error);
    }
}

JITCompiler::~JITCompiler() noexcept = default;

// Concrete global JIT instance used by the runtime
JITCompiler global_jit;

auto JITCompiler::compileFunctionFromBuilder(const std::function<llvm::Function *(llvm::Module &)> &builder) -> void * {
    std::string mname = "jit_module";
    auto m = std::make_unique<llvm::Module>(mname, context);

    llvm::Function *f = builder(*m);
    if (!f)
        return nullptr;

    llvm::Module *mptr = m.get();

    // If the module references our runtime bridge, make sure it's mapped
    if (llvm::Function *bridge = mptr->getFunction("VDLISP__call_from_jit")) {
        executionEngine->addGlobalMapping(bridge, reinterpret_cast<void *>(VDLISP__call_from_jit));
    }

    // Map helper(s) used by JITed code.
    if (llvm::Function *lookup = mptr->getFunction("VDLISP__jit_lookup_number")) {
        executionEngine->addGlobalMapping(lookup, reinterpret_cast<void *>(VDLISP__jit_lookup_number));
    }

    executionEngine->addModule(std::move(m));
    executionEngine->finalizeObject();
    void *ptr = executionEngine->getPointerToFunction(f);
    if (ptr)
        module_for_fn[ptr] = mptr;
    return ptr;
}

void JITCompiler::releaseFunctionCode(void *fnPtr) noexcept {
    if (!fnPtr)
        return;
    auto it = module_for_fn.find(fnPtr);
    if (it == module_for_fn.end())
        return;
    llvm::Module *mptr = it->second;
    try {
        auto res = executionEngine->removeModule(mptr);
        (void)res;
    } catch (...) {
    }
    for (auto jt = module_for_fn.begin(); jt != module_for_fn.end();) {
        if (jt->second == mptr)
            jt = module_for_fn.erase(jt);
        else
            ++jt;
    }
}

auto JITCompiler::getContext() noexcept -> llvm::LLVMContext & {
    return context;
}

// helper: scan an AST and collect TFUNC pointers referenced by symbol calls
static void collect_called_funcs(const vdlisp::Value &expr, std::vector<vdlisp::FuncData *> &out, vdlisp::Env *closure) {
    using namespace vdlisp;
    if (!expr)
        return;
    if (expr.get_type() == TPAIR) {
        vdlisp::PairData *pd = expr.get_pair();
        const Value &car = pd->car;
        const Value &cdr = pd->cdr;
        if (car && car.get_type() == TSYMBOL) {
            std::string name = *car.get_symbol();
            Env *e = closure;
            if (e)
                retain_env(e);
            while (e) {
                auto it = e->map.find(name);
                if (it != e->map.end()) {
                    Value v = it->second;
                    if (v && v.get_type() == TFUNC) {
                        out.push_back(v.get_func());
                    }
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
        }
        const Value *walk = &expr;
        while (*walk) {
            PairData *wpd = walk->get_pair();
            collect_called_funcs(wpd->car, out, closure);
            walk = &wpd->cdr;
        }
    }
}

auto JITCompiler::compileFuncData(vdlisp::FuncData *func) -> void * {
    if (!func)
        return nullptr;
    using namespace vdlisp;

    std::vector<FuncData *> to_compile;
    collect_called_funcs(func->body, to_compile, func->closure_env);
    for (FuncData *fd : to_compile) {
        if (fd && !fd->compiled_code && !fd->jit_failed && fd != func) {
            try {
                void *res = this->compileFuncData(fd);
                (void)res;
            } catch (...) {
                // ignore
            }
        }
    }

    std::string fname = "jit_fn_" + std::to_string(reinterpret_cast<uintptr_t>(func));
    auto builder = [func, this, fname](llvm::Module &M) -> llvm::Function * {
        return build_func_ir(func, M, this->getContext(), fname);
    };

    void *ptr = nullptr;
    try {
        ptr = this->compileFunctionFromBuilder(builder);
    } catch (const std::exception &e) {
        func->jit_failed = true;
        return nullptr;
    } catch (...) {
        func->jit_failed = true;
        return nullptr;
    }
    if (!ptr) {
        func->jit_failed = true;
        return nullptr;
    }
    func->compiled_code = ptr;
    return ptr;
}
